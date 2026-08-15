/* A small modal text editor with vim keybindings -- the first AnssOS
 * program that draws a full screen rather than scrolling log lines past.
 *
 * Three things had to exist in the kernel before this was possible, all
 * added alongside it: TIOCGWINSZ (there was no way to ask how big the
 * terminal is), O_TRUNC (writes only ever grew a file, so saving a
 * shortened buffer left the old tail behind), and an ANSI/CSI parser in
 * console/fbconsole.c (cursor addressing had nowhere to land -- escape
 * sequences were drawn as literal glyphs). Escape and the shifted symbol
 * row also had to be added to the virtio-input keymap; without them
 * there was no way to leave insert mode or type ':' in a graphical
 * window.
 *
 * Deliberately not supported, to keep this reviewable: visual mode,
 * registers/yank/put, undo, and search. The cursor is drawn as a
 * reverse-video cell (SGR 7) rather than the terminal's own cursor,
 * because that renders identically on the framebuffer console and over
 * serial.
 *
 * There's no argv in AnssOS yet (execve takes a path and nothing else),
 * so the file to edit is named from inside the editor with `:e <path>`
 * rather than on a command line. */

#include "libc.h"

#define MODE_NORMAL 0
#define MODE_INSERT 1
#define MODE_COMMAND 2

#define STATUS_ROWS 2 /* Status line + message/command line. */
#define TAB_STOP 4

struct erow {
    char *chars;
    int len;
    int cap;
};

static struct erow *rows;
static int numrows;
static int rowcap;

static int cx, cy;         /* Cursor, in file coordinates. */
static int rowoff, coloff; /* Top-left of the viewport. */
static int screenrows, screencols;
static int textrows; /* screenrows - STATUS_ROWS. */
static int mode;
static int dirty;
static int quit;
static char filename[128];
static char message[128];
static char cmdbuf[128];
static int cmdlen;
static int pending; /* A half-typed operator: 'd' or 'g', else 0. */

static struct termios orig_termios;

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

static void str_copy(char *dest, int destcap, const char *src) {
    int i = 0;
    while (src[i] != '\0' && i < destcap - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void set_message(const char *s) {
    str_copy(message, (int)sizeof(message), s);
}

/* ---------- output buffer: one write() per redraw ---------- */

struct abuf {
    char *b;
    int len;
    int cap;
};

static void ab_append(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int newcap = (ab->cap == 0) ? 4096 : ab->cap * 2;
        while (newcap < ab->len + len) {
            newcap *= 2;
        }
        char *fresh = grow(ab->b, ab->len, newcap);
        if (fresh == NULL) {
            return; /* Out of memory: drop output rather than corrupt it. */
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

/* Places the cursor at a 1-based row/column. Every line this editor draws
 * is positioned explicitly rather than separated by \r\n, because a line
 * that exactly fills the width auto-wraps to the next row -- and a
 * trailing newline after it would then advance a *second* time, drifting
 * the frame down a row per redraw until the screen scrolls and leaves a
 * stale duplicate behind. The status bar is deliberately full-width, so
 * this is the normal case here, not a corner one. */
static void ab_goto(struct abuf *ab, int row, int col) {
    ab_str(ab, "\x1b[");
    ab_int(ab, row);
    ab_str(ab, ";");
    ab_int(ab, col);
    ab_str(ab, "H");
}

/* ---------- row storage ---------- */

static int row_reserve(int want) {
    if (want <= rowcap) {
        return 0;
    }
    int newcap = rowcap == 0 ? 64 : rowcap;
    while (newcap < want) {
        newcap *= 2;
    }
    struct erow *fresh =
        grow(rows, rowcap * (int)sizeof(struct erow), newcap * (int)sizeof(struct erow));
    if (fresh == NULL) {
        return -1;
    }
    rows = fresh;
    rowcap = newcap;
    return 0;
}

static int row_set(struct erow *r, const char *s, int len) {
    if (len + 1 > r->cap) {
        int newcap = r->cap == 0 ? 32 : r->cap;
        while (newcap < len + 1) {
            newcap *= 2;
        }
        char *fresh = grow(r->chars, r->len, newcap);
        if (fresh == NULL) {
            return -1;
        }
        r->chars = fresh;
        r->cap = newcap;
    }
    if (len > 0) {
        memmove(r->chars, s, (size_t)len);
    }
    r->len = len;
    r->chars[len] = '\0';
    return 0;
}

static int row_insert(int at, const char *s, int len) {
    if (at < 0 || at > numrows || row_reserve(numrows + 1) != 0) {
        return -1;
    }
    memmove(&rows[at + 1], &rows[at], (size_t)(numrows - at) * sizeof(struct erow));
    rows[at].chars = NULL;
    rows[at].len = 0;
    rows[at].cap = 0;
    if (row_set(&rows[at], s, len) != 0) {
        return -1;
    }
    numrows++;
    dirty = 1;
    return 0;
}

static void row_delete(int at) {
    if (at < 0 || at >= numrows) {
        return;
    }
    free(rows[at].chars);
    memmove(&rows[at], &rows[at + 1], (size_t)(numrows - at - 1) * sizeof(struct erow));
    numrows--;
    dirty = 1;
}

static void buffer_free(void) {
    for (int i = 0; i < numrows; i++) {
        free(rows[i].chars);
    }
    numrows = 0;
    cx = 0;
    cy = 0;
    rowoff = 0;
    coloff = 0;
}

/* ---------- file I/O ---------- */

static int file_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* A path that doesn't exist yet is how you start a new file --
         * same as vim. It gets created on the first :w. */
        buffer_free();
        row_insert(0, "", 0);
        str_copy(filename, (int)sizeof(filename), path);
        dirty = 0;
        set_message("new file");
        return 0;
    }

    buffer_free();

    char chunk[512];
    char *line = NULL;
    int linelen = 0, linecap = 0;
    for (;;) {
        long n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                row_insert(numrows, line == NULL ? "" : line, linelen);
                linelen = 0;
                continue;
            }
            if (chunk[i] == '\r') {
                continue;
            }
            if (linelen + 1 > linecap) {
                int newcap = linecap == 0 ? 128 : linecap * 2;
                char *fresh = grow(line, linelen, newcap);
                if (fresh == NULL) {
                    close(fd);
                    set_message("out of memory reading file");
                    return -1;
                }
                line = fresh;
                linecap = newcap;
            }
            line[linelen++] = chunk[i];
        }
    }
    if (linelen > 0) {
        row_insert(numrows, line, linelen); /* Final line with no trailing newline. */
    }
    free(line);
    close(fd);

    if (numrows == 0) {
        row_insert(0, "", 0);
    }
    str_copy(filename, (int)sizeof(filename), path);
    dirty = 0;
    set_message("opened");
    return 0;
}

static int file_save(void) {
    if (filename[0] == '\0') {
        set_message("no file name -- use :e <path> first");
        return -1;
    }

    /* O_TRUNC is the load-bearing flag here: without it a save that made
     * the file shorter would leave the previous version's tail in place,
     * since the kernel's write path only ever extends a file. */
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        set_message("cannot open file for writing");
        return -1;
    }

    struct abuf out = {NULL, 0, 0};
    for (int i = 0; i < numrows; i++) {
        ab_append(&out, rows[i].chars, rows[i].len);
        ab_str(&out, "\n");
    }
    long written = out.len > 0 ? write(fd, out.b, (unsigned long)out.len) : 0;
    close(fd);
    free(out.b);

    if (written != out.len) {
        set_message("write failed");
        return -1;
    }
    dirty = 0;
    set_message("written");
    return 0;
}

/* ---------- rendering ---------- */

static void scroll_to_cursor(void) {
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + textrows) {
        rowoff = cy - textrows + 1;
    }
    if (cx < coloff) {
        coloff = cx;
    }
    if (cx >= coloff + screencols) {
        coloff = cx - screencols + 1;
    }
}

static void draw_rows(struct abuf *ab) {
    for (int y = 0; y < textrows; y++) {
        int filerow = y + rowoff;
        ab_goto(ab, y + 1, 1);
        if (filerow >= numrows) {
            ab_str(ab, "~");
        } else {
            struct erow *r = &rows[filerow];
            int len = r->len - coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > screencols) {
                len = screencols;
            }
            int cursor_here = (mode != MODE_COMMAND && filerow == cy);
            for (int i = 0; i < len; i++) {
                if (cursor_here && coloff + i == cx) {
                    ab_str(ab, "\x1b[7m");
                    ab_append(ab, &r->chars[coloff + i], 1);
                    ab_str(ab, "\x1b[0m");
                } else {
                    ab_append(ab, &r->chars[coloff + i], 1);
                }
            }
            /* Cursor sitting past the last character (end of line, or an
             * empty line) still needs to be visible -- draw it as a
             * highlighted space. */
            if (cursor_here && cx >= r->len && cx - coloff < screencols) {
                ab_str(ab, "\x1b[7m \x1b[0m");
            }
        }
        ab_str(ab, "\x1b[K");
    }
}

static void draw_status(struct abuf *ab) {
    ab_goto(ab, textrows + 1, 1);
    ab_str(ab, "\x1b[7m");
    int used = 0;

    const char *name = filename[0] != '\0' ? filename : "[no name]";
    ab_str(ab, name);
    used += (int)strlen(name);
    if (dirty) {
        ab_str(ab, " [+]");
        used += 4;
    }

    const char *modestr = mode == MODE_INSERT    ? "  -- INSERT --"
                          : mode == MODE_COMMAND ? "  -- COMMAND --"
                                                 : "";
    ab_str(ab, modestr);
    used += (int)strlen(modestr);

    /* Right-hand side: line/total and column, padded out so the inverted
     * bar spans the full width. */
    struct abuf right = {NULL, 0, 0};
    ab_int(&right, cy + 1);
    ab_str(&right, "/");
    ab_int(&right, numrows);
    ab_str(&right, " col ");
    ab_int(&right, cx + 1);

    int pad = screencols - used - right.len;
    for (int i = 0; i < pad; i++) {
        ab_str(ab, " ");
    }
    if (right.len > 0 && pad >= 0) {
        ab_append(ab, right.b, right.len);
    }
    free(right.b);
    ab_str(ab, "\x1b[0m");
}

static void draw_message(struct abuf *ab) {
    ab_goto(ab, textrows + 2, 1);
    if (mode == MODE_COMMAND) {
        ab_str(ab, ":");
        ab_append(ab, cmdbuf, cmdlen);
        ab_str(ab, "\x1b[7m \x1b[0m");
    } else {
        ab_str(ab, message);
    }
    ab_str(ab, "\x1b[K");
}

static void refresh(void) {
    scroll_to_cursor();

    struct abuf ab = {NULL, 0, 0};
    ab_str(&ab, "\x1b[?25l"); /* Hide the real cursor; we draw our own. */
    draw_rows(&ab);
    draw_status(&ab);
    draw_message(&ab);
    if (ab.len > 0) {
        write(1, ab.b, (unsigned long)ab.len);
    }
    free(ab.b);
}

/* ---------- motions and edits ---------- */

static struct erow *cur_row(void) {
    return cy < numrows ? &rows[cy] : NULL;
}

static void clamp_cx(void) {
    struct erow *r = cur_row();
    int max = r == NULL ? 0 : r->len;
    if (mode != MODE_INSERT && max > 0) {
        max = r->len - 1; /* Normal mode sits *on* a character, not past it. */
    }
    if (cx > max) {
        cx = max;
    }
    if (cx < 0) {
        cx = 0;
    }
}

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void move_word_forward(void) {
    struct erow *r = cur_row();
    if (r == NULL) {
        return;
    }
    int i = cx;
    while (i < r->len && is_word(r->chars[i])) {
        i++;
    }
    while (i < r->len && !is_word(r->chars[i])) {
        i++;
    }
    if (i >= r->len && cy + 1 < numrows) {
        cy++;
        cx = 0;
        return;
    }
    cx = i;
}

static void move_word_back(void) {
    struct erow *r = cur_row();
    if (r == NULL) {
        return;
    }
    int i = cx - 1;
    while (i > 0 && !is_word(r->chars[i])) {
        i--;
    }
    while (i > 0 && is_word(r->chars[i - 1])) {
        i--;
    }
    if (i < 0) {
        i = 0;
    }
    if (cx == 0 && cy > 0) {
        cy--;
        struct erow *prev = cur_row();
        cx = prev != NULL && prev->len > 0 ? prev->len - 1 : 0;
        return;
    }
    cx = i;
}

static void insert_char(char c) {
    struct erow *r = cur_row();
    if (r == NULL) {
        if (row_insert(numrows, "", 0) != 0) {
            return;
        }
        r = &rows[numrows - 1];
    }
    if (r->len + 2 > r->cap) {
        int newcap = r->cap == 0 ? 32 : r->cap * 2;
        char *fresh = grow(r->chars, r->len + 1, newcap);
        if (fresh == NULL) {
            return;
        }
        r->chars = fresh;
        r->cap = newcap;
    }
    memmove(&r->chars[cx + 1], &r->chars[cx], (size_t)(r->len - cx + 1));
    r->chars[cx] = c;
    r->len++;
    r->chars[r->len] = '\0';
    cx++;
    dirty = 1;
}

static void delete_char_at(int at) {
    struct erow *r = cur_row();
    if (r == NULL || at < 0 || at >= r->len) {
        return;
    }
    memmove(&r->chars[at], &r->chars[at + 1], (size_t)(r->len - at));
    r->len--;
    dirty = 1;
}

static void split_line(void) {
    struct erow *r = cur_row();
    if (r == NULL) {
        row_insert(numrows, "", 0);
        cy = numrows - 1;
        cx = 0;
        return;
    }
    if (row_insert(cy + 1, &r->chars[cx], r->len - cx) != 0) {
        return;
    }
    r = &rows[cy]; /* row_insert may have moved the array. */
    row_set(r, r->chars, cx);
    cy++;
    cx = 0;
}

static void join_with_previous(void) {
    if (cy == 0) {
        return;
    }
    struct erow *prev = &rows[cy - 1];
    struct erow *r = &rows[cy];
    int at = prev->len;
    int total = prev->len + r->len;

    char *merged = malloc((size_t)total + 1);
    if (merged == NULL) {
        return;
    }
    memcpy(merged, prev->chars, (size_t)prev->len);
    memcpy(merged + prev->len, r->chars, (size_t)r->len);
    merged[total] = '\0';
    row_set(prev, merged, total);
    free(merged);

    row_delete(cy);
    cy--;
    cx = at;
}

static void delete_word(void) {
    struct erow *r = cur_row();
    if (r == NULL || cx >= r->len) {
        return;
    }
    int end = cx;
    while (end < r->len && is_word(r->chars[end])) {
        end++;
    }
    while (end < r->len && !is_word(r->chars[end])) {
        end++;
    }
    memmove(&r->chars[cx], &r->chars[end], (size_t)(r->len - end + 1));
    r->len -= (end - cx);
    dirty = 1;
}

/* ---------- command line (:...) ---------- */

static void run_command(void) {
    cmdbuf[cmdlen] = '\0';

    if (strcmp(cmdbuf, "q") == 0) {
        if (dirty) {
            set_message("unsaved changes -- :q! to discard, :wq to save");
        } else {
            quit = 1;
        }
    } else if (strcmp(cmdbuf, "q!") == 0) {
        quit = 1;
    } else if (strcmp(cmdbuf, "w") == 0) {
        file_save();
    } else if (strcmp(cmdbuf, "wq") == 0 || strcmp(cmdbuf, "x") == 0) {
        if (file_save() == 0) {
            quit = 1;
        }
    } else if (strncmp(cmdbuf, "e ", 2) == 0) {
        if (dirty) {
            set_message("unsaved changes -- :w first, or :q! to discard");
        } else {
            file_open(&cmdbuf[2]);
        }
    } else if (strncmp(cmdbuf, "w ", 2) == 0) {
        str_copy(filename, (int)sizeof(filename), &cmdbuf[2]);
        file_save();
    } else {
        set_message("unknown command");
    }

    cmdlen = 0;
    mode = MODE_NORMAL;
    clamp_cx();
}

/* ---------- key dispatch ---------- */

static void key_normal(char c) {
    if (pending == 'd') {
        pending = 0;
        if (c == 'd') {
            row_delete(cy);
            if (numrows == 0) {
                row_insert(0, "", 0);
            }
            if (cy >= numrows) {
                cy = numrows - 1;
            }
            clamp_cx();
        } else if (c == 'w') {
            delete_word();
            clamp_cx();
        }
        return;
    }
    if (pending == 'g') {
        pending = 0;
        if (c == 'g') {
            cy = 0;
            cx = 0;
        }
        return;
    }

    switch (c) {
        case 'h':
            if (cx > 0) {
                cx--;
            }
            break;
        case 'l': {
            struct erow *r = cur_row();
            if (r != NULL && cx + 1 < r->len) {
                cx++;
            }
            break;
        }
        case 'j':
            if (cy + 1 < numrows) {
                cy++;
                clamp_cx();
            }
            break;
        case 'k':
            if (cy > 0) {
                cy--;
                clamp_cx();
            }
            break;
        case '0':
            cx = 0;
            break;
        case '$': {
            struct erow *r = cur_row();
            cx = (r != NULL && r->len > 0) ? r->len - 1 : 0;
            break;
        }
        case 'w':
            move_word_forward();
            clamp_cx();
            break;
        case 'b':
            move_word_back();
            clamp_cx();
            break;
        case 'G':
            cy = numrows > 0 ? numrows - 1 : 0;
            clamp_cx();
            break;
        case 'g':
            pending = 'g';
            break;
        case 'd':
            pending = 'd';
            break;
        case 'x':
            delete_char_at(cx);
            clamp_cx();
            break;
        case 'i':
            mode = MODE_INSERT;
            break;
        case 'a': {
            struct erow *r = cur_row();
            if (r != NULL && r->len > 0) {
                cx++;
            }
            mode = MODE_INSERT;
            break;
        }
        case 'A': {
            struct erow *r = cur_row();
            cx = r != NULL ? r->len : 0;
            mode = MODE_INSERT;
            break;
        }
        case 'o':
            row_insert(cy + 1, "", 0);
            cy++;
            cx = 0;
            mode = MODE_INSERT;
            break;
        case 'O':
            row_insert(cy, "", 0);
            cx = 0;
            mode = MODE_INSERT;
            break;
        case ':':
            mode = MODE_COMMAND;
            cmdlen = 0;
            message[0] = '\0';
            break;
        default:
            break;
    }
}

static void key_insert(char c) {
    if (c == 0x1b) {
        mode = MODE_NORMAL;
        if (cx > 0) {
            cx--; /* vim leaves the cursor on the last inserted character. */
        }
        clamp_cx();
        return;
    }
    if (c == '\r' || c == '\n') {
        split_line();
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (cx > 0) {
            delete_char_at(cx - 1);
            cx--;
        } else {
            join_with_previous();
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < TAB_STOP; i++) {
            insert_char(' ');
        }
        return;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        insert_char(c);
    }
}

static void key_command(char c) {
    if (c == 0x1b) {
        mode = MODE_NORMAL;
        cmdlen = 0;
        return;
    }
    if (c == '\r' || c == '\n') {
        run_command();
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (cmdlen > 0) {
            cmdlen--;
        } else {
            mode = MODE_NORMAL;
        }
        return;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c < 127 && cmdlen < (int)sizeof(cmdbuf) - 1) {
        cmdbuf[cmdlen++] = c;
    }
}

/* ---------- setup / teardown ---------- */

static int raw_mode_on(void) {
    if (tcgetattr(0, &orig_termios) != 0) {
        return -1;
    }
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(0, TCSANOW, &raw);
}

static void raw_mode_off(void) {
    tcsetattr(0, TCSANOW, &orig_termios);
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
    textrows = screenrows - STATUS_ROWS;
    if (textrows < 1) {
        textrows = 1;
    }
}

int main(void) {
    if (raw_mode_on() != 0) {
        printf("scarf: cannot put the terminal in raw mode\n");
        exit(1);
    }
    get_window_size();

    row_insert(0, "", 0);
    dirty = 0;
    set_message("scarf -- :e <file> to open, i to insert, Esc for normal, :w :q");

    while (!quit) {
        refresh();

        char c;
        if (read(0, &c, 1) != 1) {
            continue;
        }
        if (mode == MODE_NORMAL) {
            key_normal(c);
        } else if (mode == MODE_INSERT) {
            key_insert(c);
        } else {
            key_command(c);
        }
    }

    /* Leave the terminal exactly as it was found: normal video, cursor
     * visible, screen cleared, cooked mode back on -- otherwise the
     * shell prompt inherits reverse video and an invisible cursor. */
    write(1, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h", 18);
    raw_mode_off();
    exit(0);
}
