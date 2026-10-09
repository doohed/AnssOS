#ifndef DRIVERS_USB_USB_KBD_H
#define DRIVERS_USB_USB_KBD_H

#include "usb.h"

#include <stdint.h>

/* USB keyboards, through the HID boot protocol: the fixed 8-byte report
 * every USB keyboard supports for BIOS setup screens (modifier bits plus
 * up to six pressed keys), so there's no HID report descriptor to parse.
 * Key repeat is done here, from the timer, since the boot protocol only
 * reports what's held. */

/* Called by xhci.c for each newly enumerated device with its whole
 * configuration descriptor. Claims it if it has a boot keyboard
 * interface; returns 0 if claimed. */
int usb_kbd_probe(struct usb_device *dev, const uint8_t *config, uint16_t length);

/* How many USB keyboards are plugged in and working. */
int usb_kbd_count(void);

/* Non-blocking: the next byte typed on any USB keyboard (polling the
 * controllers as it goes), or -1. Same bytes as every other keyboard --
 * see drivers/keymap.h. */
int usb_kbd_poll_char(void);

#endif
