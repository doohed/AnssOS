#include "input.h"
#include "serial.h"
#include "usb/usb_kbd.h"
#include "virtio/virtio_input.h"

int input_poll_char(void) {
    int c = virtio_input_poll_char();
    if (c < 0) {
        c = usb_kbd_poll_char();
    }
    if (c < 0) {
        c = serial_poll_char();
    }
    return c;
}
