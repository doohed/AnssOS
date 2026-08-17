/* A userland shell -- unlike kernel/src/shell/shell.c (kernel-resident,
 * so nothing exec()'able into a tile.c pane), this is a real ELF
 * program: `run sh` from the kernel shell, or spawned as a pane by
 * userland/tile.c. Deliberately narrower than the kernel shell -- see
 * "Not included" at the bottom -- built for running programs and light
 * navigation inside a pane, not full file management (the kernel shell
 * remains the tool for that).
 *
 * Command parsing (split_command/tokenize/copy_bounded) and program
 * resolution (resolve_program's bare-name-then-/bin search) port
 * shell.c's own already-proven logic, rewritten against syscalls
 * instead of kernel-internal vfs_* calls.
 *
 * Input is the one real design difference from the kernel shell:
 * shell.c's read_line() polls hardware directly and blocks. This has to
 * work whether fd 0 is the physical console *or* a pipe (M19) -- a pipe
 * read never blocks (see kernel/src/exec/pipe.h's own comment on why:
 * ring-0 spins can't yield to the process that would produce the data).
 * So raw mode is set unconditionally at startup (own echo, own
 * backspace handling, like userland/play.c/scarf.c already do) and
 * read_line() below reads one byte at a time in a loop that treats a 0
 * return as "no data yet, keep looping" -- a ring-3 spin, safely
 * preemptible, letting sibling pane processes actually run between
 * iterations when piped. Over the real console this never actually
 * returns 0 (see sys_read_impl's raw-mode path) -- the loop shape is
 * identical either way, just blocks in one case and not the other. */

#include "libc.h"

#define LINE_MAX 256
#define CMD_MAX 32
#define PATH_MAX 256

/* Set when tile.c spawned this sh as a pane (see its own comment on the
 * "--tile-pane" argv flag it passes). Guards against running a
 * full-screen ANSI program from inside a pane: TIOCGWINSZ has no
 * concept of panes and always reports the *physical* screen size, so a
 * program like scarf/play would draw with absolute coordinates across
 * the whole screen -- not just render oddly in its own pane, but
 * overwrite tile's header and every other pane too. There's no general
 * fix short of a real per-pane ANSI virtual terminal (out of scope --
 * see docs/tile.md); this is a guard rail, not a workaround. */
static int g_in_pane = 0;

/* Names known to draw full-screen ANSI UI (cursor addressing, ESC[K,
 * reverse video) rather than just scrolling text -- the actual thing
 * that's unsafe inside a pane, not "any program". A real fix would
 * detect this some other way; a short list is honest about being one. */
static const char *UNSAFE_IN_PANE[] = {"scarf", "play"};
#define UNSAFE_IN_PANE_COUNT ((int)(sizeof(UNSAFE_IN_PANE) / sizeof(UNSAFE_IN_PANE[0])))

static int basename_is(const char *path, const char *name) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return strcmp(base, name) == 0;
}

static int unsafe_in_pane(const char *path) {
    for (int i = 0; i < UNSAFE_IN_PANE_COUNT; i++) {
        if (basename_is(path, UNSAFE_IN_PANE[i])) {
            return 1;
        }
    }
    return 0;
}

static int resolve_program(const char *name, char *out, size_t out_size) {
    int has_slash = 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/') {
            has_slash = 1;
            break;
        }
    }
    if (has_slash) {
        int fd = open(name, O_RDONLY);
        if (fd < 0) {
            return 0;
        }
        close(fd);
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return 1;
    }

    int fd = open(name, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return 1;
    }

    char path[PATH_MAX];
    strncpy(path, "/bin/", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    size_t n = strlen(path);
    strncpy(path + n, name, sizeof(path) - n - 1);
    path[sizeof(path) - 1] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

/* Whitespace-tokenizes `line` in place (writes NULs at the boundaries),
 * matching shell.c's tokenize() -- no quoting, same limitation as the
 * line reader itself. Returns argc. */
static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < max_args) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

static void run_program(const char *path, int argc, char *argv[]) {
    int pid = fork();
    if (pid < 0) {
        printf("sh: fork failed\n");
        return;
    }
    if (pid == 0) {
        char *const empty_argv[] = {(char *)path, NULL};
        execve(path, argc > 0 ? argv : empty_argv);
        printf("sh: %s: exec failed\n", path);
        exit(1);
    }
    int status = -1;
    waitpid(pid, &status);
}

static void cmd_ls(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (d == NULL) {
        printf("ls: %s: cannot open\n", path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        printf("  %s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    }
    closedir(d);
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("cat: %s: cannot open\n", argv[1]);
        return;
    }
    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (unsigned long)n);
    }
    close(fd);
}

static void cmd_write(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: write <file> <text...>\n");
        return;
    }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("write: %s: cannot open\n", argv[1]);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            write(fd, " ", 1);
        }
        write(fd, argv[i], strlen(argv[i]));
    }
    close(fd);
}

static void cmd_create(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: create dir|file <name>\n");
        return;
    }
    if (strcmp(argv[1], "dir") == 0) {
        if (mkdir(argv[2]) != 0) {
            printf("create: mkdir failed\n");
        }
    } else if (strcmp(argv[1], "file") == 0) {
        int fd = open(argv[2], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("create: open failed\n");
        } else {
            close(fd);
        }
    } else {
        printf("usage: create dir|file <name>\n");
    }
}

static void cmd_help(void) {
    printf(
        "sh builtins: cd, pwd, ls, cat, write, create, echo, clear, help\n"
        "anything else runs as a program (bare name searches cwd then /bin)\n"
        "not implemented (no unlink/rename/sync syscalls exist yet): "
        "delete, copy, move, sync -- use the kernel shell for those\n");
}

/* Returns 1 on a normal line (which may be empty -- a blank Enter), or 0
 * on EOF (stdin closed with nothing more coming, e.g. tile.c tore down
 * this pane) -- an explicit status rather than overloading an empty
 * `buf` for both, since a blank line and "no more input ever" need
 * different handling in main()'s loop. */
static int read_line(char *buf, size_t max_len) {
    size_t len = 0;
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n < 0) {
            buf[len] = '\0';
            return 0;
        }
        if (n == 0) {
            continue; /* No byte yet (only happens when piped) -- keep spinning. */
        }
        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                printf("\b \b");
            }
            continue;
        }
        if (len + 1 < max_len) {
            buf[len++] = c;
            putchar(c);
        }
    }
    buf[len] = '\0';
    return 1;
}

int main(int main_argc, char **main_argv) {
    for (int i = 1; i < main_argc; i++) {
        if (strcmp(main_argv[i], "--tile-pane") == 0) {
            g_in_pane = 1;
        }
    }

    struct termios orig, raw;
    int have_orig = tcgetattr(0, &orig) == 0;
    if (have_orig) {
        raw = orig;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }

    char line[LINE_MAX];
    char cwd[PATH_MAX];
    for (;;) {
        getcwd(cwd, sizeof(cwd));
        printf("sh:%s> ", cwd);

        if (!read_line(line, sizeof(line))) {
            break; /* EOF -- e.g. tile.c closed this pane's stdin. */
        }
        if (line[0] == '\0') {
            continue; /* Blank Enter. */
        }

        char *argv[16];
        int argc = tokenize(line, argv, 16);
        if (argc == 0) {
            continue;
        }

        if (strcmp(argv[0], "cd") == 0) {
            if (chdir(argc > 1 ? argv[1] : "/") != 0) {
                printf("cd: %s: no such directory\n", argc > 1 ? argv[1] : "/");
            }
        } else if (strcmp(argv[0], "pwd") == 0) {
            printf("%s\n", cwd);
        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc, argv);
        } else if (strcmp(argv[0], "cat") == 0) {
            cmd_cat(argc, argv);
        } else if (strcmp(argv[0], "write") == 0) {
            cmd_write(argc, argv);
        } else if (strcmp(argv[0], "create") == 0) {
            cmd_create(argc, argv);
        } else if (strcmp(argv[0], "echo") == 0) {
            for (int i = 1; i < argc; i++) {
                printf("%s%s", i > 1 ? " " : "", argv[i]);
            }
            printf("\n");
        } else if (strcmp(argv[0], "clear") == 0) {
            write(1, "\x1b[2J\x1b[H", 7);
        } else if (strcmp(argv[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(argv[0], "exit") == 0) {
            break;
        } else {
            char path[PATH_MAX];
            if (!resolve_program(argv[0], path, sizeof(path))) {
                printf("sh: %s: not found\n", argv[0]);
            } else if (g_in_pane && unsafe_in_pane(path)) {
                printf(
                    "sh: %s: draws full-screen ANSI, which corrupts the whole "
                    "display from inside a tile pane -- run it directly (not "
                    "under tile) instead\n",
                    argv[0]);
            } else {
                run_program(path, argc, argv);
            }
        }
    }

    if (have_orig) {
        tcsetattr(0, TCSANOW, &orig);
    }
    exit(0);
}
