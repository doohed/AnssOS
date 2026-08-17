/* A fixed-grid tiling terminal multiplexer: spawns N independent `sh`
 * (userland/sh.c) processes, each piped (M19, exec/pipe.h) into its own
 * screen cell, switchable with a tmux-style Ctrl-b prefix.
 *
 * Deliberately bounded scope (see the plan this was built from):
 *   - a fixed 1/2x1/2x2 grid (up to 4 panes), not a dynamic resizable
 *     tree -- that's exactly the feature userland/scarf.c tried and
 *     reverted (docs/scarf.md), and fixed-grid sidesteps it entirely.
 *   - each pane's "virtual terminal" is scrollback-only, no ANSI
 *     parsing: sh never emits cursor-addressing escapes (it prints text
 *     and erases with literal "\b \b", same as kernel/src/shell/shell.c
 *     itself), so there's nothing to interpret, just \r/\n/backspace
 *     bookkeeping. Running scarf/play *as* a pane needs a real per-pane
 *     ANSI virtual terminal and is explicitly out of scope here.
 *   - redraw only happens for panes that actually produced new output,
 *     not on a fixed timer -- unlike play.c's spectrum, which needed
 *     near-continuous redraws to look reactive.
 *
 * Reuses the same abuf/ab_str/ab_int/ab_goto output-buffer conventions
 * userland/scarf.c and userland/play.c already established (one
 * write() per redraw, every line positioned explicitly with
 * ESC[row;colH) rather than inventing new ones -- see either of their
 * own comments on why. Since every pane's cell is always written at a
 * fixed, padded width (never relying on ESC[K), redrawing just the
 * columns belonging to one dirty pane never touches its neighbor --
 * the exact problem scarf's reverted splits ran into, sidestepped by
 * never depending on line-clearing at all. */

#include "libc.h"

#include <stdint.h>

#define MAX_PANES 4
#define SCROLLBACK_LINES 32
#define LINE_MAX 128
#define READ_CHUNK 256

static int screencols, screenrows;

/* ---------- small helpers (no realloc/snprintf in this libc) ---------- */

static void *grow(void *old, int oldn, int newn) {
    char *fresh = malloc((size_t)newn);
    if (fresh == NULL) {
        return NULL;
    }
    if (old != NULL) {
        memcpy(fresh, old, (size_t)(oldn < newn ? oldn : newn));
        free(old);
    }
    return fresh;
}

struct abuf {
    char *b;
    int len;
    int cap;
};

static void ab_append(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int newcap = (ab->cap == 0) ? 1024 : ab->cap * 2;
        while (newcap < ab->len + len) {
            newcap *= 2;
        }
        char *fresh = grow(ab->b, ab->len, newcap);
        if (fresh == NULL) {
            return;
        }
        ab->b = fresh;
        ab->cap = newcap;
    }
    memcpy(ab->b + ab->len, s, (size_t)len);
    ab->len += len;
}

static void ab_str(struct abuf *ab, const char *s) {
    ab_append(ab, s, (int)strlen(s));
}

static void ab_int(struct abuf *ab, int v) {
    char tmp[16];
    int i = 0;
    if (v < 0) {
        ab_str(ab, "-");
        v = -v;
    }
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        ab_append(ab, &tmp[--i], 1);
    }
}

static void ab_repeat(struct abuf *ab, char c, int n) {
    for (int i = 0; i < n; i++) {
        ab_append(ab, &c, 1);
    }
}

static void ab_goto(struct abuf *ab, int row, int col) {
    ab_str(ab, "\x1b[");
    ab_int(ab, row);
    ab_str(ab, ";");
    ab_int(ab, col);
    ab_str(ab, "H");
}

/* Writes exactly `w` columns -- `s` truncated if longer, space-padded
 * if shorter. Never relies on ESC[K (see the file comment on why).
 *
 * Takes an explicit `len` rather than calling strlen(s) itself: `s` is
 * sometimes another struct abuf's own buffer (draw_pane()'s `label`),
 * and an abuf is a raw byte run tracked by `.len`, not a NUL-terminated
 * C string -- strlen() on one reads whatever garbage byte pattern
 * happens to follow in that malloc'd block (e.g. leftover content from
 * a previous free()'d allocation the userland allocator recycled the
 * same block for). Found exactly this way: a pane's label bled the
 * tail of the *header's* old text in after it, mid-boot-test. */
static void ab_field(struct abuf *ab, const char *s, int len, int w) {
    if (len > w) {
        len = w;
    }
    if (len > 0) {
        ab_append(ab, s, len);
    }
    ab_repeat(ab, ' ', w - len);
}

static void get_window_size(void) {
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        screencols = ws.ws_col;
        screenrows = ws.ws_row;
    } else {
        screencols = 80;
        screenrows = 24;
    }
    if (screencols < 40) {
        screencols = 40;
    }
    if (screenrows < 12) {
        screenrows = 12;
    }
}

/* ---------- panes ---------- */

struct pane {
    int pid;
    int in_w, out_r; /* tile's own fds: write keystrokes, read output */
    int alive;

    char lines[SCROLLBACK_LINES][LINE_MAX];
    int next;  /* ring buffer: next slot to write */
    int count; /* how many of lines[] are valid, up to SCROLLBACK_LINES */

    char cur[LINE_MAX];
    int cur_len;

    int dirty;
};

static struct pane panes[MAX_PANES];
static int pane_count;
static int focused;

static void pane_push_line(struct pane *p) {
    p->cur[p->cur_len] = '\0';
    strncpy(p->lines[p->next], p->cur, LINE_MAX - 1);
    p->lines[p->next][LINE_MAX - 1] = '\0';
    p->next = (p->next + 1) % SCROLLBACK_LINES;
    if (p->count < SCROLLBACK_LINES) {
        p->count++;
    }
    p->cur_len = 0;
}

static void pane_feed(struct pane *p, const char *data, int n) {
    for (int i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            pane_push_line(p);
        } else if (c == '\r') {
            /* Dropped -- sh.c only ever emits \n, see the file comment. */
        } else if (c == '\b' || c == 0x7F) {
            if (p->cur_len > 0) {
                p->cur_len--;
            }
        } else if (p->cur_len + 1 < LINE_MAX) {
            p->cur[p->cur_len++] = c;
        }
    }
    p->dirty = 1;
}

/* Spawns `path` with a fresh pair of pipes wired to its stdin/stdout,
 * exactly the pattern M19's plan doc lays out: pipe() twice, fork(),
 * the child closes the ends it doesn't need and use_as_stdio()s the
 * rest *before* execve(), the parent closes its own unneeded ends and
 * keeps the other two. `idx` is this pane's own index into the global
 * `panes[]` -- see the loop below for why the child needs it. Returns 0
 * on success. */
static int spawn_pane(struct pane *p, int idx, const char *path) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
        return -1;
    }
    int pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        close(in[1]);
        close(out[0]);
        /* fork() shallow-copies tile's *entire* open_files[] table, not
         * just the pipe pair just created above -- every already-spawned
         * pane's in_w/out_r comes along too. This child never references
         * those by name, so it would never close them, silently holding
         * a phantom reference that keeps an earlier pane's pipe from
         * ever reaching zero refs -- found exactly this way, as a real
         * hang: tile's shutdown path closed pane 0's stdin and waited
         * forever, because pane 1's child was quietly still holding
         * pane 0's write end open too. Real Unix has this exact problem,
         * which is what close-on-exec exists to solve; there's no
         * O_CLOEXEC here, so close explicitly instead. */
        for (int j = 0; j < idx; j++) {
            close(panes[j].in_w);
            close(panes[j].out_r);
        }
        use_as_stdio(in[0], out[1]);
        /* Tells sh.c it's running piped, not on the real console --
         * see its own comment on why that matters (full-screen ANSI
         * programs like scarf/play corrupt the whole physical screen
         * if run from inside a pane, since TIOCGWINSZ has no concept
         * of panes and reports the full physical size regardless). */
        char *const argv[] = {(char *)path, "--tile-pane", NULL};
        execve(path, argv);
        exit(1); /* unreachable on success */
    }
    close(in[0]);
    close(out[1]);

    p->pid = pid;
    p->in_w = in[1];
    p->out_r = out[0];
    p->alive = 1;
    p->next = 0;
    p->count = 0;
    p->cur_len = 0;
    p->dirty = 1;
    return 0;
}

/* ---------- grid layout ---------- */

#define ROW_HEADER 1
#define GRID_TOP 2

static int grid_cols, grid_rows, cell_w, cell_h;

static void compute_grid(void) {
    grid_cols = (pane_count >= 2) ? 2 : 1;
    grid_rows = (pane_count >= 3) ? 2 : 1;
    cell_w = screencols / grid_cols;
    cell_h = (screenrows - GRID_TOP + 1) / grid_rows;
}

static int cell_row0(int idx) {
    return GRID_TOP + (idx / grid_cols) * cell_h;
}

static int cell_col0(int idx) {
    return 1 + (idx % grid_cols) * cell_w;
}

static void draw_header(struct abuf *ab) {
    ab_goto(ab, ROW_HEADER, 1);
    ab_str(ab, "\x1b[7m");
    struct abuf left = {NULL, 0, 0};
    ab_str(&left, " tile -- pane ");
    ab_int(&left, focused + 1);
    ab_str(&left, "/");
    ab_int(&left, pane_count);
    const char *right = "Ctrl-b <digit> switch  Ctrl-b q quit ";
    int pad = screencols - left.len - (int)strlen(right);
    if (pad < 1) {
        pad = 1;
    }
    ab_append(ab, left.b, left.len);
    ab_repeat(ab, ' ', pad);
    ab_str(ab, right);
    ab_str(ab, "\x1b[0m");
    free(left.b);
}

static void draw_pane(struct abuf *ab, int idx) {
    struct pane *p = &panes[idx];
    int row0 = cell_row0(idx);
    int col0 = cell_col0(idx);
    int content_w = cell_w - (grid_cols > 1 && (idx % grid_cols) < grid_cols - 1 ? 1 : 0);
    int content_h = cell_h - 1; /* One row spent on the label. */

    int is_focused = (idx == focused);
    ab_goto(ab, row0, col0);
    if (is_focused) {
        ab_str(ab, "\x1b[7m");
    }
    struct abuf label = {NULL, 0, 0};
    ab_str(&label, " pane ");
    ab_int(&label, idx + 1);
    if (!p->alive) {
        ab_str(&label, " [exited]");
    }
    ab_field(ab, label.b, label.len, content_w);
    if (is_focused) {
        ab_str(ab, "\x1b[0m");
    }
    free(label.b);

    /* Grows top-down like a real terminal, not bottom-pinned: a pane's
     * cell is nearly full-screen tall (few panes, one shared header
     * row), so bottom-aligning a few lines of prompt text left most of
     * it looking like an empty box with text stuck at the very bottom
     * -- reported as "looks weird" and confirmed against a mockup.
     * `shown` history lines (oldest first) starting right after the
     * label row, then the in-progress line, then blank out whatever's
     * left below that. Once there's enough scrollback to fill the
     * whole cell (hist == shown), this naturally lands the current line
     * on the cell's last row with zero blank rows left -- real
     * terminal-scrolling behavior, not a separate case. */
    int shown = content_h - 1;
    if (shown < 0) {
        shown = 0;
    }
    int hist = p->count < shown ? p->count : shown;

    int start = (p->next - hist + SCROLLBACK_LINES) % SCROLLBACK_LINES;
    int row = row0 + 1;
    for (int r = 0; r < hist; r++) {
        const char *line = p->lines[(start + r) % SCROLLBACK_LINES];
        ab_goto(ab, row, col0);
        ab_field(ab, line, (int)strlen(line), content_w);
        row++;
    }
    if (shown > 0) {
        ab_goto(ab, row, col0);
        ab_field(ab, p->cur, p->cur_len, content_w);
        row++;
    }
    for (; row < row0 + cell_h; row++) {
        ab_goto(ab, row, col0);
        ab_field(ab, "", 0, content_w);
    }

    /* Column separator, if this cell isn't in the rightmost column. */
    if (grid_cols > 1 && (idx % grid_cols) < grid_cols - 1) {
        for (int r = 0; r < cell_h; r++) {
            ab_goto(ab, row0 + r, col0 + content_w);
            ab_str(ab, "|");
        }
    }

    p->dirty = 0;
}

static int header_dirty = 1;

/* Nothing gets written at all unless something actually changed --
 * the main loop below calls this every iteration regardless (it has
 * to, to stay responsive), so drawing unconditionally here would mean
 * a full-width header redraw hundreds of times a second even while
 * genuinely idle. Found exactly this way: Ctrl-b q looked like it
 * hung, but it was actually a busy-spin of identical header redraws
 * drowning everything else out -- the same cost scarf.md warns a
 * full-framebuffer flush has under TCG, self-inflicted here by
 * skipping the same dirty-tracking discipline draw_pane() already
 * has. */
static void redraw(int force_all) {
    struct abuf ab = {NULL, 0, 0};
    int any = 0;
    if (force_all || header_dirty) {
        ab_str(&ab, "\x1b[?25l");
        draw_header(&ab);
        header_dirty = 0;
        any = 1;
    }
    for (int i = 0; i < pane_count; i++) {
        if (force_all || panes[i].dirty) {
            if (!any) {
                ab_str(&ab, "\x1b[?25l");
                any = 1;
            }
            draw_pane(&ab, i);
        }
    }
    if (any) {
        write(1, ab.b, ab.len);
    }
    free(ab.b);
}

/* ---------- main loop ---------- */

static int raw_mode_on(struct termios *orig) {
    if (tcgetattr(0, orig) != 0) {
        return -1;
    }
    struct termios raw = *orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(0, TCSANOW, &raw);
}

int main(int argc, char **argv) {
    pane_count = 2;
    if (argc > 1) {
        int n = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            n = n * 10 + (*p - '0');
        }
        if (n >= 1 && n <= MAX_PANES) {
            pane_count = n;
        }
    }

    struct termios orig;
    if (raw_mode_on(&orig) != 0) {
        printf("tile: cannot put the terminal in raw mode\n");
        exit(1);
    }
    get_window_size();
    compute_grid();
    write(1, "\x1b[2J", 4);

    for (int i = 0; i < pane_count; i++) {
        if (spawn_pane(&panes[i], i, "/bin/sh") != 0) {
            printf("tile: failed to spawn pane %d\n", i + 1);
            exit(1);
        }
    }
    focused = 0;

    redraw(1);

    int prefix = 0;
    int running = pane_count;
    while (running > 0) {
        int key = poll_key();
        if (key >= 0) {
            if (prefix) {
                prefix = 0;
                if (key >= '1' && key <= '9' && (key - '1') < pane_count) {
                    focused = key - '1';
                    redraw(1);
                } else if (key == 'q') {
                    break;
                }
                /* Anything else after the prefix is just dropped. */
            } else if (key == 0x02) { /* Ctrl-b */
                prefix = 1;
            } else if (panes[focused].alive) {
                char c = (char)key;
                write(panes[focused].in_w, &c, 1);
            }
        }

        for (int i = 0; i < pane_count; i++) {
            struct pane *p = &panes[i];
            if (!p->alive) {
                continue;
            }
            char buf[READ_CHUNK];
            long n = read(p->out_r, buf, sizeof(buf));
            if (n > 0) {
                pane_feed(p, buf, (int)n);
            } else if (n < 0) {
                p->alive = 0;
                p->dirty = 1;
                int status = -1;
                waitpid(p->pid, &status);
                running--;
            }
        }

        redraw(0);
    }

    /* Tear down whatever's still running: closing a pane's stdin pipe
     * is what makes sh.c's read_line() see EOF and exit on its own
     * (see M19's design -- there's no signals to do this any other
     * way). */
    for (int i = 0; i < pane_count; i++) {
        if (panes[i].alive) {
            close(panes[i].in_w);
            int status = -1;
            waitpid(panes[i].pid, &status);
        }
    }

    write(1, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h", 18);
    tcsetattr(0, TCSANOW, &orig);
    printf("tile: done\n");
    exit(0);
}
