#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

/* SATA disks through AHCI controllers (PCI class 01.06, prog-if 01; QEMU's
 * q35 machine has one built in). Each port with a disk on it is
 * registered as a block device (drivers/block.h) named sata0, sata1...;
 * CD/DVD drives and port multipliers are skipped. Polled, one command at
 * a time. Taking a port over and identifying its drive doesn't touch the
 * drive's data. Returns how many disks it registered. */
int ahci_init(void);

#endif
