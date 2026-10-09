#include "keymap.h"

#include <stddef.h>
#include <stdint.h>

struct special_key {
    uint16_t code;
    const char *seq;
};

/* Keys with no single byte come out as the escape sequences a VT100-style
 * terminal sends for them, exactly what a program reading a serial
 * terminal already sees. */
static const struct special_key SPECIAL_KEYS[] = {
    {102, "\x1b[H"},  /* Home */
    {103, "\x1b[A"},  /* Up */
    {104, "\x1b[5~"}, /* Page Up */
    {105, "\x1b[D"},  /* Left */
    {106, "\x1b[C"},  /* Right */
    {107, "\x1b[F"},  /* End */
    {108, "\x1b[B"},  /* Down */
    {109, "\x1b[6~"}, /* Page Down */
    {111, "\x1b[3~"}, /* Delete */
};

/* Linux key codes -> ASCII. 0 means "no mapping, drop the key".
 *
 * Escape (code 1) matters more than it looks: it's the only way out of
 * insert mode in scarf, and without it modal editing is impossible on a
 * keyboard. The keypad (55, 71-83, 96, 98) types its digits and symbols,
 * as if Num Lock were on. */
static const char KEYMAP_LOWER[99] = {
    [1] = 0x1b,  [2] = '1',  [3] = '2',  [4] = '3',   [5] = '4',  [6] = '5',   [7] = '6',
    [8] = '7',   [9] = '8',  [10] = '9', [11] = '0',  [12] = '-', [13] = '=',  [14] = '\b',
    [15] = '\t', [16] = 'q', [17] = 'w', [18] = 'e',  [19] = 'r', [20] = 't',  [21] = 'y',
    [22] = 'u',  [23] = 'i', [24] = 'o', [25] = 'p',  [26] = '[', [27] = ']',  [28] = '\n',
    [30] = 'a',  [31] = 's', [32] = 'd', [33] = 'f',  [34] = 'g', [35] = 'h',  [36] = 'j',
    [37] = 'k',  [38] = 'l', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z',
    [45] = 'x',  [46] = 'c', [47] = 'v', [48] = 'b',  [49] = 'n', [50] = 'm',  [51] = ',',
    [52] = '.',  [53] = '/', [55] = '*', [57] = ' ',  [71] = '7', [72] = '8',  [73] = '9',
    [74] = '-',  [75] = '4', [76] = '5', [77] = '6',  [78] = '+', [79] = '1',  [80] = '2',
    [81] = '3',  [82] = '0', [83] = '.', [96] = '\n', [98] = '/',
};

/* The shifted half of the same layout. Codes absent here fall back to
 * KEYMAP_LOWER (with a-z uppercased), so an unshifted key never stops
 * working just because its shifted form is unlisted. */
static const char KEYMAP_UPPER[54] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',  [7] = '^',  [8] = '&',
    [9] = '*',  [10] = '(', [11] = ')', [12] = '_', [13] = '+', [26] = '{', [27] = '}',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|', [51] = '<', [52] = '>', [53] = '?',
};

const char *keymap_bytes(uint16_t code, int mods) {
    for (size_t i = 0; i < sizeof(SPECIAL_KEYS) / sizeof(SPECIAL_KEYS[0]); i++) {
        if (SPECIAL_KEYS[i].code == code) {
            return SPECIAL_KEYS[i].seq;
        }
    }
    if (code >= sizeof(KEYMAP_LOWER)) {
        return NULL;
    }

    char c = KEYMAP_LOWER[code];
    int is_letter = c >= 'a' && c <= 'z';
    int shift = (mods & KEYMOD_SHIFT) != 0;
    if ((mods & KEYMOD_CTRL) && is_letter) {
        c = (char)(c - 'a' + 1); /* Ctrl-A..Ctrl-Z -> 0x01..0x1a, as a terminal sends. */
    } else if (is_letter) {
        if (shift != ((mods & KEYMOD_CAPS) != 0)) {
            c = (char)(c - 'a' + 'A');
        }
    } else if (shift && code < sizeof(KEYMAP_UPPER) && KEYMAP_UPPER[code] != '\0') {
        c = KEYMAP_UPPER[code];
    }
    if (c == '\0') {
        return NULL;
    }

    static char one[2];
    one[0] = c;
    one[1] = '\0';
    return one;
}
