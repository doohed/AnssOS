#ifndef DRIVERS_USB_USB_STORAGE_H
#define DRIVERS_USB_USB_STORAGE_H

#include "usb.h"

#include <stdint.h>

/* USB sticks and disks: the mass-storage class over Bulk-Only Transport
 * with SCSI commands -- what nearly all of them speak. Each one is
 * registered as a block device (drivers/block.h) named usb0, usb1...
 * (UAS-only devices, mostly fast external SSDs, also offer this.)
 *
 * Called by xhci.c for each newly enumerated device that no other class
 * driver claimed. Returns 0 if it took it. */
int usb_storage_probe(struct usb_device *dev, const uint8_t *config, uint16_t length);

#endif
