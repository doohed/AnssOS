#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t revision, prog_if, subclass, class_code;
    uint8_t header_type;
    uint32_t bar[6]; /* Only valid for header_type 0x00 (normal devices). */
};

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Brute-force scans all 256 buses x 32 slots x (1 or 8) functions over the */
/* legacy 0xCF8/0xCFC config-space ports. Fine for QEMU's flat topology; */
/* a real machine would want to walk the ACPI MCFG table for ECAM instead */
/* (out of scope for now -- see README roadmap). */
void pci_enumerate(void);

/* Looks up a previously enumerated device. Returns NULL if not found or */
/* if pci_enumerate() hasn't run yet. */
const struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Like pci_find_device(), but returns the `index`'th match (0-based) --
 * needed for e.g. virtio-input, where QEMU exposes keyboard, mouse, and
 * tablet devices under the identical vendor:device id and the caller has
 * to inspect each one's config to tell them apart. Returns NULL past the
 * last match. */
const struct pci_device *pci_find_device_nth(uint16_t vendor_id, uint16_t device_id, int index);

/* Re-lists whatever pci_enumerate() already found, via kprintf, without
 * rescanning -- used by the shell's `lspci` builtin. */
void pci_print_devices(void);

#endif
