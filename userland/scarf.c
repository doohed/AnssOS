/* A small modal text editor with vim keybindings and a file-explorer
 * sidebar -- the first AnssOS program that draws a full screen rather
 * than scrolling log lines past.
 *
 * Three things had to exist in the kernel before this was possible, all
 * added alongside it: TIOCGWINSZ (there was no way to ask how big the
 * terminal is), O_TRUNC (writes only ever grew a file, so saving a
 * shortened buffer left the old tail behind), and an ANSI/CSI parser in
 * console/fbconsole.c (cursor addressing had nowhere to land -- escape
 * sequences were drawn as literal glyphs). Escape, the shifted symbol
 * row, and Ctrl also had to be added to the virtio-input keymap; without
 * them there was no way to leave insert mode, type ':', or press a
 * Ctrl-shortcut in a graphical window.
 *
 * The sidebar is built on M15's opendir()/readdir() -- it's the first
 * thing in the tree that actually uses them for something other than a
 * self-test.
 *
 * Window splits were tried and removed: tiled panes can't use ESC[K to
 * clear a line (it would erase the pane beside it), so every cell had to
 * be padded with spaces, which meant ~20 KB of output per keystroke.
 * The layout here is deliberately one editor pane reaching the right
 * edge, so it can clear with ESC[K and write only as many bytes as the
 * text actually occupies. The sidebar does still pad (something is to
 * its right), so it's redrawn only when it changes rather than on every
 * keystroke.
 *
 * Deliberately not supported, to keep this reviewable: visual mode,
 * registers/yank/put, undo, and search. The cursor is drawn as a
 * reverse-video cell (SGR 7) rather than the terminal's own cursor,
 * because that renders identically on the framebuffer console and over
 * serial.
 *
 * Takes an optional path: a directory opens the sidebar there, a file
 * opens straight into the editor. `scarf .` therefore edits in whatever
 * directory the shell was sitting in, since a task now inherits its
 * launcher's cwd rather than always starting at the root. */

#include "libc.h"

#define MODE_NORMAL 0
#define MODE_INSERT 1
#define MODE_COMMAND 2

#define FOCUS_EDITOR 0
#define FOCUS_SIDEBAR 1

#define MAX_ENTRIES 128
#define NAME_MAX 64
#define PATH_MAX 128
#define SIDEBAR_W 28
#define TAB_STOP 4

#define CTRL_B 0x02 /* Toggle the sidebar -- same key VS Code uses. */
#define CTRL_E 0x05 /* Move focus between the sidebar and the editor. */

struct erow {
    char *chars;
    int len;
    int cap;
};

struct entry {
    char name[NAME_MAX];
    int isdir;
};

static struct erow *rows;
static int numrows;
static int rowcap;
static char filename[PATH_MAX];
static int dirty;

static int cx, cy;         /* Cursor, in file coordinates. */
static int rowoff, coloff; /* Top-left of the editor viewport. */

static int screenrows, screencols;
static int mode;
static int focus;
static int quit;
static char message[PATH_MAX];
static char cmdbuf[PATH_MAX];
static int cmdlen;
static int pending; /* A half-typed operator: 'd' or 'g', else 0. */

static int sidebar_visible = 1;
static int sidebar_w;
static struct entry entries[MAX_ENTRIES];
static int nentries;
static int sel;    /* Selected row in the sidebar. */
static int seloff; /* First visible sidebar row. */
static char cwd_path[PATH_MAX] = "/";
static int sidebar_dirty = 1; /* Repaint the sidebar on the next refresh. */

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

/* "/" + "foo" -> "/foo";  "/a" + "foo" -> "/a/foo". */
static void path_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] != '\0' && n < cap - 1; i++) {
        out[n++] = dir[i];
    }
    if (n > 0 && out[n - 1] != '/' && n < cap - 1) {
        out[n++] = '/';
    }
    for (int i = 0; name[i] != '\0' && n < cap - 1; i++) {
        out[n++] = name[i];
    }
    out[n] = '\0';
}

/* Strips the last component in place: "/a/b" -> "/a", "/a" -> "/". */
static void path_parent(char *p) {
    int last = -1;
    for (int i = 0; p[i] != '\0'; i++) {
        if (p[i] == '/') {
            last = i;
        }
    }
    if (last <= 0) {
        p[0] = '/';
        p[1] = '\0';
        return;
    }
    p[last] = '\0';
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

/* Places the cursor at a 1-based row/column. Every line is positioned
 * explicitly rather than separated by \r\n: a line that exactly fills the
 * width auto-wraps to the next row, and a trailing newline after it would
 * then advance a *second* time, drifting the frame down a row per redraw
 * until the screen scrolls and leaves a stale duplicate behind. */
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
        row_insert(numrows, line, linelen); /* Final line, no trailing newline. */
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
        set_message("no file name -- use :w <path>");
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
    sidebar_dirty = 1; /* A new file may have appeared in the listing. */
    set_message("written");
    return 0;
}

/* ---------- sidebar ---------- */

static void sidebar_load(void) {
    nentries = 0;
    sel = 0;
    seloff = 0;
    sidebar_dirty = 1;

    /* ".." first, so going back up is always the top entry -- the VFS
     * itself resolves "..", this is only the listing convention. */
    if (strcmp(cwd_path, "/") != 0) {
        str_copy(entries[nentries].name, NAME_MAX, "..");
        entries[nentries].isdir = 1;
        nentries++;
    }

    DIR *d = opendir(cwd_path);
    if (d == NULL) {
        set_message("cannot open directory");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nentries < MAX_ENTRIES) {
        str_copy(entries[nentries].name, NAME_MAX, e->d_name);
        entries[nentries].isdir = (e->d_type == DT_DIR);
        nentries++;
    }
    closedir(d);
}

static void sidebar_activate(void) {
    if (sel < 0 || sel >= nentries) {
        return;
    }
    struct entry *e = &entries[sel];

    if (e->isdir) {
        if (strcmp(e->name, "..") == 0) {
            path_parent(cwd_path);
        } else {
            char next[PATH_MAX];
            path_join(next, sizeof(next), cwd_path, e->name);
            str_copy(cwd_path, sizeof(cwd_path), next);
        }
        sidebar_load();
        return;
    }

    if (dirty) {
        set_message("unsaved changes -- :w first, or :e! to discard");
        return;
    }
    char full[PATH_MAX];
    path_join(full, sizeof(full), cwd_path, e->name);
    file_open(full);
    focus = FOCUS_EDITOR;
    sidebar_dirty = 1;
}

/* ---------- rendering ---------- */

static int editor_x(void) {
    return sidebar_visible ? sidebar_w + 1 : 0; /* +1 for the separator column. */
}

static int editor_w(void) {
    int w = screencols - editor_x();
    return w > 1 ? w : 1;
}

static int editor_rows(void) {
    int h = screenrows - 2; /* Editor status line, then the message line. */
    return h > 1 ? h : 1;
}

static void scroll_to_cursor(void) {
    int textrows = editor_rows();
    int width = editor_w();
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + textrows) {
        rowoff = cy - textrows + 1;
    }
    if (cx < coloff) {
        coloff = cx;
    }
    if (cx >= coloff + width) {
        coloff = cx - width + 1;
    }
}

/* The editor pane reaches the right edge, so it can clear each line with
 * ESC[K instead of padding out to the full width -- the difference
 * between writing a few hundred bytes per redraw and ~20 KB. */
static void draw_editor(struct abuf *ab) {
    int textrows = editor_rows();
    int x = editor_x();
    int width = editor_w();
    int show_cursor = (focus == FOCUS_EDITOR && mode != MODE_COMMAND);

    for (int y = 0; y < textrows; y++) {
        int filerow = y + rowoff;
        ab_goto(ab, y + 1, x + 1);

        if (filerow >= numrows) {
            ab_str(ab, "~");
        } else {
            struct erow *r = &rows[filerow];
            int len = r->len - coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > width) {
                len = width;
            }
            int written = 0;
            for (int i = 0; i < len; i++) {
                int is_cursor = show_cursor && filerow == cy && coloff + i == cx;
                if (is_cursor) {
                    ab_str(ab, "\x1b[7m");
                }
                ab_append(ab, &r->chars[coloff + i], 1);
                if (is_cursor) {
                    ab_str(ab, "\x1b[0m");
                }
                written++;
            }
            /* Cursor past the last character (end of line, or an empty
             * line) still needs to be visible. */
            if (show_cursor && filerow == cy && cx >= r->len && cx - coloff == written &&
                written < width) {
                ab_str(ab, "\x1b[7m \x1b[0m");
            }
        }
        ab_str(ab, "\x1b[K");
    }
}

static void draw_status(struct abuf *ab) {
    struct abuf left = {NULL, 0, 0};
    ab_str(&left, filename[0] != '\0' ? filename : "[no name]");
    if (dirty) {
        ab_str(&left, " [+]");
    }
    if (mode == MODE_INSERT) {
        ab_str(&left, "  -- INSERT --");
    }
    if (focus == FOCUS_SIDEBAR) {
        ab_str(&left, "  -- FILES --");
    }

    struct abuf right = {NULL, 0, 0};
    ab_int(&right, cy + 1);
    ab_str(&right, "/");
    ab_int(&right, numrows);
    ab_str(&right, " col ");
    ab_int(&right, cx + 1);

    int x = editor_x();
    int width = editor_w();
    ab_goto(ab, screenrows - 1, x + 1);
    ab_str(ab, "\x1b[7m");

    int written = 0;
    for (int i = 0; i < left.len && written < width; i++, written++) {
        ab_append(ab, &left.b[i], 1);
    }
    int pad = width - written - right.len;
    for (int i = 0; i < pad; i++, written++) {
        ab_str(ab, " ");
    }
    if (pad >= 0) {
        for (int i = 0; i < right.len && written < width; i++, written++) {
            ab_append(ab, &right.b[i], 1);
        }
    }
    while (written < width) {
        ab_str(ab, " ");
        written++;
    }

    ab_str(ab, "\x1b[0m");
    free(left.b);
    free(right.b);
}

/* Padded to its own width, because the editor pane sits to its right --
 * ESC[K here would wipe the text. Hence sidebar_dirty: this only runs
 * when the listing, selection, or focus actually changed, not on every
 * keystroke typed into the editor. */
static void draw_sidebar(struct abuf *ab) {
    int visible = screenrows - 2; /* Header row, then entries, above the message line. */
    if (visible < 1) {
        visible = 1;
    }

    if (sel < seloff) {
        seloff = sel;
    }
    if (sel >= seloff + visible - 1) {
        seloff = sel - visible + 2;
    }
    if (seloff < 0) {
        seloff = 0;
    }

    /* Header: the directory being listed, right-truncated to fit. */
    ab_goto(ab, 1, 1);
    ab_str(ab, "\x1b[7m");
    int written = 0;
    for (int i = 0; cwd_path[i] != '\0' && written < sidebar_w; i++, written++) {
        ab_append(ab, &cwd_path[i], 1);
    }
    while (written < sidebar_w) {
        ab_str(ab, " ");
        written++;
    }
    ab_str(ab, "\x1b[0m");

    for (int y = 1; y < visible; y++) {
        int idx = seloff + y - 1;
        ab_goto(ab, y + 1, 1);

        written = 0;
        int selected = (idx == sel && idx < nentries);
        if (selected) {
            ab_str(ab, focus == FOCUS_SIDEBAR ? "\x1b[7m" : "\x1b[7m");
        }
        if (idx < nentries) {
            struct entry *e = &entries[idx];
            /* A marker rather than a colour: this console has no SGR
             * colour support, only reverse video. */
            const char *mark = e->isdir ? "> " : "  ";
            for (int i = 0; mark[i] != '\0' && written < sidebar_w; i++, written++) {
                ab_append(ab, &mark[i], 1);
            }
            for (int i = 0; e->name[i] != '\0' && written < sidebar_w; i++, written++) {
                ab_append(ab, &e->name[i], 1);
            }
            if (e->isdir && written < sidebar_w) {
                ab_str(ab, "/");
                written++;
            }
        }
        while (written < sidebar_w) {
            ab_str(ab, " ");
            written++;
        }
        if (selected) {
            ab_str(ab, "\x1b[0m");
        }
    }

    for (int y = 0; y < screenrows - 1; y++) {
        ab_goto(ab, y + 1, sidebar_w + 1);
        ab_str(ab, "|");
    }
}

static void draw_message(struct abuf *ab) {
    ab_goto(ab, screenrows, 1);
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
    if (sidebar_visible && sidebar_dirty) {
        draw_sidebar(&ab);
        sidebar_dirty = 0;
    }
    draw_editor(&ab);
    draw_status(&ab);
    draw_message(&ab);
    if (ab.len > 0) {
        write(1, ab.b, (unsigned long)ab.len);
    }
    free(ab.b);
}

static void toggle_sidebar(void) {
    sidebar_visible = !sidebar_visible;
    if (sidebar_visible) {
        sidebar_dirty = 1;
    } else {
        focus = FOCUS_EDITOR;
    }
    /* The editor pane changes width either way, and when the sidebar
     * goes away it has to repaint the columns the sidebar occupied --
     * ESC[K on each editor line does that, since the pane now starts at
     * column 0. Clearing the screen keeps the separator from lingering. */
    write(1, "\x1b[2J", 4);
    coloff = 0;
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
    if (cx == 0 && cy > 0) {
        cy--;
        struct erow *prev = cur_row();
        cx = prev != NULL && prev->len > 0 ? prev->len - 1 : 0;
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
            set_message("unsaved changes -- :w first, or :e! to discard");
        } else {
            file_open(&cmdbuf[2]);
        }
    } else if (strncmp(cmdbuf, "e! ", 3) == 0) {
        file_open(&cmdbuf[3]);
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

static void key_sidebar(char c) {
    switch (c) {
        case 'j':
            if (sel + 1 < nentries) {
                sel++;
                sidebar_dirty = 1;
            }
            break;
        case 'k':
            if (sel > 0) {
                sel--;
                sidebar_dirty = 1;
            }
            break;
        case 'g':
            sel = 0;
            sidebar_dirty = 1;
            break;
        case 'G':
            sel = nentries > 0 ? nentries - 1 : 0;
            sidebar_dirty = 1;
            break;
        case '\r':
        case '\n':
        case 'l':
            sidebar_activate();
            break;
        case 'h':
        case '-':
            path_parent(cwd_path);
            sidebar_load();
            break;
        case 'r':
            sidebar_load(); /* Re-read: files the editor created show up. */
            set_message("refreshed");
            break;
        case 0x1b:
            focus = FOCUS_EDITOR;
            sidebar_dirty = 1;
            break;
        case ':':
            focus = FOCUS_EDITOR;
            sidebar_dirty = 1;
            mode = MODE_COMMAND;
            cmdlen = 0;
            message[0] = '\0';
            break;
        default:
            break;
    }
}

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
    if (screenrows < 4) {
        screenrows = 4;
    }

    sidebar_w = SIDEBAR_W;
    if (sidebar_w > screencols / 3) {
        sidebar_w = screencols / 3; /* Never let the sidebar crowd out the text. */
    }
    if (sidebar_w < 8) {
        sidebar_w = 8;
    }
}

int main(int argc, char **argv) {
    if (raw_mode_on() != 0) {
        printf("scarf: cannot put the terminal in raw mode\n");
        exit(1);
    }
    get_window_size();

    row_insert(0, "", 0);
    dirty = 0;

    /* Start where the shell was rather than at the root -- getcwd()
     * resolves the inherited cwd to an absolute path, so the sidebar
     * header shows a real location instead of the literal "." that may
     * have been passed as the argument. */
    if (getcwd(cwd_path, sizeof(cwd_path)) != 0) {
        str_copy(cwd_path, sizeof(cwd_path), "/");
    }

    /* argv[1], if given, is either a directory to browse or a file to
     * open. Which one is decided by trying opendir() on it: that fails
     * on a regular file, which is exactly the test needed. */
    focus = FOCUS_SIDEBAR;
    if (argc > 1) {
        char target[PATH_MAX];
        if (argv[1][0] == '/') {
            str_copy(target, sizeof(target), argv[1]);
        } else {
            path_join(target, sizeof(target), cwd_path, argv[1]);
        }
        /* chdir() is the directory test, not opendir(): opendir() is just
         * open(path, O_RDONLY), which succeeds on a regular file too, so
         * it can't tell the two apart. chdir() rejects anything that
         * isn't a directory. It also canonicalises as a side effect --
         * getcwd() afterwards turns `scarf .` into "/docs" rather than
         * displaying the literal "/docs/.". */
        if (chdir(target) == 0) {
            getcwd(cwd_path, sizeof(cwd_path));
        } else {
            file_open(target);
            focus = FOCUS_EDITOR;
        }
    }

    sidebar_load();
    set_message("scarf -- Ctrl-b files, Ctrl-e switch pane, Enter open, i insert, :w :q");

    write(1, "\x1b[2J", 4);
    while (!quit) {
        refresh();

        char c;
        if (read(0, &c, 1) != 1) {
            continue;
        }

        /* The two pane shortcuts work from anywhere except while a
         * command line or an insert is in progress, where the raw byte
         * belongs to whatever's being typed. */
        if (mode == MODE_NORMAL && c == CTRL_B) {
            toggle_sidebar();
            continue;
        }
        if (mode == MODE_NORMAL && c == CTRL_E) {
            if (sidebar_visible) {
                focus = (focus == FOCUS_EDITOR) ? FOCUS_SIDEBAR : FOCUS_EDITOR;
                sidebar_dirty = 1;
            }
            continue;
        }

        if (mode == MODE_COMMAND) {
            key_command(c);
        } else if (mode == MODE_INSERT) {
            key_insert(c);
        } else if (focus == FOCUS_SIDEBAR) {
            key_sidebar(c);
        } else {
            key_normal(c);
        }
    }

    /* Leave the terminal exactly as it was found: normal video, cursor
     * visible, screen cleared, cooked mode back on -- otherwise the
     * shell prompt inherits reverse video and an invisible cursor. */
    write(1, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h", 18);
    raw_mode_off();
    exit(0);
}
