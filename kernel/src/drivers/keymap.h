#ifndef DRIVERS_KEYMAP_H
#define DRIVERS_KEYMAP_H

#include <stdint.h>

#define KEYMOD_SHIFT 1
#define KEYMOD_CTRL 2
#define KEYMOD_CAPS 4 /* Caps Lock on: letters flip case. */

/* US QWERTY. Turns a key press, as a Linux key code (the kernel's
 * input-event-codes.h -- what virtio-input reports, and what the USB
 * keyboard driver converts HID usages to), into the bytes a VT100-style
 * terminal would send for it: one byte for most keys, ESC [ ... for
 * arrows, Home/End, Delete and Page Up/Down, Ctrl-A..Ctrl-Z as 0x01-0x1a.
 * Returns a NUL-terminated string (valid until the next call), or NULL
 * for keys that produce nothing (modifiers, function keys, ...). Every
 * keyboard driver goes through this, so a program sees identical bytes
 * whichever keyboard -- or the serial console -- it's reading. */
const char *keymap_bytes(uint16_t code, int mods);

#endif
