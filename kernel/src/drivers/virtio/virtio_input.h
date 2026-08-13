#ifndef DRIVERS_VIRTIO_VIRTIO_INPUT_H
#define DRIVERS_VIRTIO_VIRTIO_INPUT_H

/* virtio-input keyboard driver. QEMU exposes keyboard/mouse/tablet all
 * under the same vendor:device PCI id, so this walks every match via
 * pci_find_device_nth() and picks the one whose VIRTIO_INPUT_CFG_ID_NAME
 * config string contains "Keyboard". pci_enumerate() must have already
 * run. Returns 0 on success, -1 if no virtio-input keyboard is present. */
int virtio_input_init(void);

/* Non-blocking: returns the next translated ASCII character, or -1 if
 * nothing is pending right now. '\n' for Enter, '\b' for Backspace.
 * Keys without an ASCII mapping (arrows, function keys, ...) are
 * silently dropped -- call again to get the next one. */
int virtio_input_poll_char(void);

#endif
