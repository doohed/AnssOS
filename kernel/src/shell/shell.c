#include "shell.h"
#include "../arch/x86_64/io.h"
#include "../arch/x86_64/usermode.h"
#include "../console/fbconsole.h"
#include "../drivers/pci.h"
#include "../drivers/pit.h"
#include "../drivers/serial.h"
#include "../drivers/virtio/virtio_blk.h"
#include "../drivers/virtio/virtio_input.h"
#include "../exec/elf.h"
#include "../fs/blkfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define LINE_MAX 256
#define CMD_MAX 32

static struct vnode *cwd;

typedef void (*builtin_fn)(const char *args);

struct builtin {
    const char *name;
    const char *help;
    builtin_fn fn;
};

static void cmd_help(const char *args);
static void cmd_echo(const char *args);
static void cmd_clear(const char *args);
static void cmd_uname(const char *args);
static void cmd_meminfo(const char *args);
static void cmd_lspci(const char *args);
static void cmd_uptime(const char *args);
static void cmd_crash(const char *args);
static void cmd_reboot(const char *args);
static void cmd_halt(const char *args);
static void cmd_create(const char *args);
static void cmd_delete(const char *args);
static void cmd_copy(const char *args);
static void cmd_move(const char *args);
static void cmd_cd(const char *args);
static void cmd_pwd(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_write(const char *args);
static void cmd_sync(const char *args);
static void cmd_run(const char *args);

static const struct builtin BUILTINS[] = {
    {"help", "list commands", cmd_help},
    {"echo", "echo <text> -- print text back", cmd_echo},
    {"clear", "clear the screen", cmd_clear},
    {"uname", "print kernel/arch info", cmd_uname},
    {"meminfo", "print physical memory allocator stats", cmd_meminfo},
    {"lspci", "list PCI devices found at boot", cmd_lspci},
    {"uptime", "print time elapsed since interrupts were enabled", cmd_uptime},
    {"crash", "deliberately trigger a #DE to test exception handling", cmd_crash},
    {"reboot", "reset the machine", cmd_reboot},
    {"halt", "halt the CPU forever", cmd_halt},
    {"create", "create dir|file <name> -- make a directory or empty file", cmd_create},
    {"delete", "delete dir|file <name> -- remove one (directories recursively)", cmd_delete},
    {"copy", "copy dir|file <src> <dest> -- duplicate a file or directory tree", cmd_copy},
    {"move", "move dir|file <src> <dest> -- rename/relocate", cmd_move},
    {"cd", "cd [path] -- change directory (no path -- go to /)", cmd_cd},
    {"pwd", "print the current directory", cmd_pwd},
    {"ls", "ls [path] -- list a directory's contents", cmd_ls},
    {"cat", "cat <file> -- print a file's contents", cmd_cat},
    {"write", "write <file> <text...> -- overwrite a file's contents", cmd_write},
    {"sync", "flush the filesystem to disk now (also happens automatically)", cmd_sync},
    {"run", "run <path> -- execute a static ELF64 program in ring 3", cmd_run},
};
#define BUILTIN_COUNT ((int)(sizeof(BUILTINS) / sizeof(BUILTINS[0])))

static char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Blocks until Enter, polling two independent input sources each
 * iteration: virtio_input_poll_char() (needs a real graphical window
 * with keyboard focus -- nothing in a headless/-display-none setup) and
 * serial_poll_char() (whatever's typed into the host terminal via
 * `-serial stdio`, a real serial console and the one that actually works
 * headless). Whichever has a byte ready wins. Recognizes both the
 * translated codes virtio_input.c produces ('\n' for Enter, '\b' for
 * Backspace) and the raw bytes a serial terminal in raw mode commonly
 * sends instead ('\r' for Enter, DEL/0x7F for Backspace). Echoes each
 * character as typed; backspace erases the last one both on the
 * framebuffer console (fbconsole_putc('\b')) and, via "\b \b", on the
 * serial terminal. */
static void read_line(char *buf, size_t max_len) {
    size_t len = 0;
    for (;;) {
        int c = virtio_input_poll_char();
        if (c < 0) {
            c = serial_poll_char();
        }
        if (c < 0) {
            asm volatile("pause");
            continue;
        }

        if (c == '\n' || c == '\r') {
            kprintf("\n");
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                kprintf("\b \b");
            }
            continue;
        }
        if (len + 1 < max_len) {
            buf[len++] = (char)c;
            kprintf("%c", c);
        }
    }
    buf[len] = '\0';
}

/* Splits `line` into a lowercased command word (for case-insensitive
 * matching) and a pointer to the remaining text, unmodified, for
 * commands like `echo` that need to preserve the caller's casing. */
static void split_command(const char *line, char *cmd_out, size_t cmd_out_size,
                          const char **args_out) {
    while (*line == ' ') {
        line++;
    }

    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && i + 1 < cmd_out_size) {
        cmd_out[i] = lower_char(line[i]);
        i++;
    }
    cmd_out[i] = '\0';

    const char *rest = line + i;
    while (*rest != '\0' && *rest != ' ') {
        rest++; /* Command word longer than cmd_out_size -- skip the remainder of it. */
    }
    while (*rest == ' ') {
        rest++;
    }
    *args_out = rest;
}

static int strcmp_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (lower_char(*a) != lower_char(*b)) {
            return 1;
        }
        a++;
        b++;
    }
    return *a != *b;
}

static void copy_bounded(char *dest, const char *src, size_t dest_size) {
    size_t len = strlen(src);
    if (len >= dest_size) {
        len = dest_size - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

/* Splits `buf` in place into up to `max_tokens` space-separated tokens
 * (writes NULs at the boundaries), returning pointers into it. No
 * quoting -- names/paths can't contain spaces, same limitation as the
 * line reader itself. */
static int tokenize(char *buf, const char *tokens[], int max_tokens) {
    int count = 0;
    char *p = buf;
    while (*p != '\0' && count < max_tokens) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        tokens[count++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    return count;
}

static void cmd_help(const char *args) {
    (void)args;
    kprintf("Available commands:\n");
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        kprintf("  %s - %s\n", BUILTINS[i].name, BUILTINS[i].help);
    }
}

static void cmd_echo(const char *args) {
    kprintf("%s\n", args);
}

static void cmd_clear(const char *args) {
    (void)args;
    fbconsole_clear();
}

static void cmd_uname(const char *args) {
    (void)args;
    kprintf("AnssOS x86_64 (UEFI/Limine, virtio-only)\n");
}

static void cmd_meminfo(const char *args) {
    (void)args;
    uint64_t free_pages = pmm_free_page_count();
    uint64_t total_pages = pmm_total_page_count();
    kprintf("%lu / %lu pages free (%lu / %lu MiB)\n", free_pages, total_pages,
            (free_pages * PMM_PAGE_SIZE) / (1024 * 1024),
            (total_pages * PMM_PAGE_SIZE) / (1024 * 1024));
}

static void cmd_lspci(const char *args) {
    (void)args;
    pci_print_devices();
}

static void cmd_uptime(const char *args) {
    (void)args;
    /* kprintf has no field-width support (see drivers/serial.h), so this
     * skips trying to zero-pad a "seconds.milliseconds" split and just
     * reports both plainly. */
    uint64_t ms = pit_uptime_ms();
    kprintf("%lus (%lu ms, %lu ticks @ %u Hz)\n", ms / 1000, ms, pit_ticks(), (uint32_t)PIT_HZ);
}

static void cmd_crash(const char *args) {
    (void)args;
    kprintf("Triggering a deliberate #DE...\n");

    /* Forces a real DIV instruction with a zero divisor -- inline asm so */
    /* the compiler can't optimize away or reorder around the fault the */
    /* way it could with `1 / (volatile int)0` at -O2. Vector 0, #DE, no */
    /* error code. Never returns: isr_handler halts after dumping state. */
    asm volatile(
        "xor %%edx, %%edx\n"
        "mov $1, %%eax\n"
        "xor %%ecx, %%ecx\n"
        "div %%ecx\n"
        :
        :
        : "eax", "ecx", "edx");

    kprintf("unreachable if the #DE handler fired correctly.\n");
}

/* Deliberate triple fault: load a null IDT (limit 0, so no interrupt has
 * anywhere valid to go), mask hardware IRQs first so only the explicit
 * int3 below can trigger it, then trigger one. With no valid IDT entry
 * for the resulting #GP, the CPU faults trying to handle the fault
 * itself (#GP -> nowhere to go -> double fault -> nowhere to go -> triple
 * fault), which every x86 CPU treats as a full reset -- unlike chipset-
 * specific registers (8042 pulse, ICH9's 0xCF9), this needs nothing
 * beyond the CPU itself. Confirmed via QEMU's QMP `query-status`
 * (`{"status": "shutdown", "running": false}` immediately after) that
 * this really does trigger a reset -- if it then looks "stuck" instead
 * of rebooting back into firmware, check for -no-reboot on the QEMU
 * command line, which turns that reset into a permanent pause. */
static void cmd_reboot(const char *args) {
    (void)args;
    kprintf("Resetting...\n");

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    asm volatile("cli");
    asm volatile("lidt %0" : : "m"(null_idt));
    asm volatile("int $0x03");

    for (;;) {
        asm volatile("hlt"); /* Unreachable if the triple fault above worked. */
    }
}

static void cmd_halt(const char *args) {
    (void)args;
    kprintf("Halting.\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

/* Auto-persists after a successful mutation (silently -- no extra
 * output beyond whatever the vfs_ / blkfs_save() calls themselves print
 * on failure) so changes survive a reboot without the user needing to
 * remember `sync`. A no-op if there's no virtio-blk device at all. */
static void autosave(void) {
    blkfs_save();
}

static void cmd_create(const char *args) {
    char buf[LINE_MAX];
    copy_bounded(buf, args, sizeof(buf));
    const char *tokens[2];
    if (tokenize(buf, tokens, 2) < 2) {
        kprintf("usage: create dir|file <name>\n");
        return;
    }
    int result;
    if (strcmp_ci(tokens[0], "dir") == 0) {
        result = vfs_mkdir(cwd, tokens[1]);
    } else if (strcmp_ci(tokens[0], "file") == 0) {
        result = vfs_create_file(cwd, tokens[1]);
    } else {
        kprintf("usage: create dir|file <name>\n");
        return;
    }
    if (result == 0) {
        autosave();
    }
}

static void cmd_delete(const char *args) {
    char buf[LINE_MAX];
    copy_bounded(buf, args, sizeof(buf));
    const char *tokens[2];
    if (tokenize(buf, tokens, 2) < 2) {
        kprintf("usage: delete dir|file <name>\n");
        return;
    }
    if (strcmp_ci(tokens[0], "dir") != 0 && strcmp_ci(tokens[0], "file") != 0) {
        kprintf("usage: delete dir|file <name>\n");
        return;
    }
    if (vfs_remove(cwd, tokens[1], cwd) == 0) {
        autosave();
    }
}

static void cmd_copy(const char *args) {
    char buf[LINE_MAX];
    copy_bounded(buf, args, sizeof(buf));
    const char *tokens[3];
    if (tokenize(buf, tokens, 3) < 3) {
        kprintf("usage: copy dir|file <src> <dest>\n");
        return;
    }
    if (strcmp_ci(tokens[0], "dir") != 0 && strcmp_ci(tokens[0], "file") != 0) {
        kprintf("usage: copy dir|file <src> <dest>\n");
        return;
    }
    if (vfs_copy(cwd, tokens[1], tokens[2]) == 0) {
        autosave();
    }
}

static void cmd_move(const char *args) {
    char buf[LINE_MAX];
    copy_bounded(buf, args, sizeof(buf));
    const char *tokens[3];
    if (tokenize(buf, tokens, 3) < 3) {
        kprintf("usage: move dir|file <src> <dest>\n");
        return;
    }
    if (strcmp_ci(tokens[0], "dir") != 0 && strcmp_ci(tokens[0], "file") != 0) {
        kprintf("usage: move dir|file <src> <dest>\n");
        return;
    }
    if (vfs_move(cwd, tokens[1], tokens[2]) == 0) {
        autosave();
    }
}

static void cmd_cd(const char *args) {
    if (args[0] == '\0') {
        cwd = vfs_root();
        return;
    }
    struct vnode *target = vfs_resolve(cwd, args);
    if (target == NULL) {
        kprintf("cd: %s: no such directory\n", args);
        return;
    }
    if (target->type != VNODE_DIR) {
        kprintf("cd: %s: not a directory\n", args);
        return;
    }
    cwd = target;
}

static void cmd_pwd(const char *args) {
    (void)args;
    char path[256];
    vfs_path(cwd, path, sizeof(path));
    kprintf("%s\n", path);
}

static void cmd_ls(const char *args) {
    vfs_list(cwd, args[0] == '\0' ? NULL : args);
}

static void cmd_cat(const char *args) {
    if (args[0] == '\0') {
        kprintf("usage: cat <file>\n");
        return;
    }
    vfs_cat(cwd, args);
}

static void cmd_write(const char *args) {
    char buf[LINE_MAX];
    copy_bounded(buf, args, sizeof(buf));

    char *p = buf;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        kprintf("usage: write <file> <text...>\n");
        return;
    }
    char *name_start = p;
    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p == ' ') {
        *p = '\0';
        p++;
    }
    while (*p == ' ') {
        p++;
    }

    if (vfs_write_file(cwd, name_start, p, 0) == 0) {
        autosave();
    }
}

static void cmd_sync(const char *args) {
    (void)args;
    if (!virtio_blk_is_ready()) {
        kprintf("sync: no virtio-blk device -- nothing to persist to\n");
        return;
    }
    if (blkfs_save() == 0) {
        kprintf("Filesystem synced.\n");
    }
}

static void cmd_run(const char *args) {
    if (args[0] == '\0') {
        kprintf("usage: run <path>\n");
        return;
    }

    struct vnode *node = vfs_resolve(cwd, args);
    if (node == NULL || node->type != VNODE_FILE) {
        kprintf("run: %s: no such file\n", args);
        return;
    }

    struct usertask task;
    if (elf_load(node->data, node->size, &task) != 0) {
        return;
    }

    int exit_status;
    enter_usermode(&task, &exit_status); /* sys_exit()/the fault path already reports the result */
    (void)exit_status;
}

void shell_run(void) {
    cwd = vfs_root();
    kprintf("\nType 'help' for a list of commands.\n");

    char line[LINE_MAX];
    for (;;) {
        char prompt_path[256];
        vfs_path(cwd, prompt_path, sizeof(prompt_path));
        kprintf("AnssOS:%s> ", prompt_path);
        read_line(line, sizeof(line));

        char cmd[CMD_MAX];
        const char *args;
        split_command(line, cmd, sizeof(cmd), &args);

        if (cmd[0] == '\0') {
            continue;
        }

        int found = 0;
        for (int i = 0; i < BUILTIN_COUNT; i++) {
            if (strcmp(cmd, BUILTINS[i].name) == 0) {
                BUILTINS[i].fn(args);
                found = 1;
                break;
            }
        }
        if (!found) {
            kprintf("unknown command: %s (try 'help')\n", cmd);
        }
    }
}
