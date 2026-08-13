#include "pci.h"
#include "serial.h"
#include "../arch/x86_64/io.h"

#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(slot & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outb((uint16_t)(PCI_CONFIG_DATA + (offset & 3)), value);
}

static const char *class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00:
            return "Unclassified";
        case 0x01:
            return "Mass Storage";
        case 0x02:
            return "Network";
        case 0x03:
            return "Display";
        case 0x04:
            return "Multimedia";
        case 0x05:
            return "Memory";
        case 0x06:
            return "Bridge";
        case 0x0C:
            return "Serial Bus";
        default:
            return "Other";
    }
}

static void print_device(const struct pci_device *dev) {
    kprintf("PCI %u:%u.%u  %x:%x  class %x.%x (%s)\n", dev->bus, dev->slot, dev->func,
            dev->vendor_id, dev->device_id, dev->class_code, dev->subclass,
            class_name(dev->class_code));
}

static void pci_probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_config_read16(bus, slot, func, 0x00);
    if (vendor_id == 0xFFFF || device_count >= PCI_MAX_DEVICES) {
        return;
    }

    struct pci_device *dev = &devices[device_count++];
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_config_read16(bus, slot, func, 0x02);
    dev->revision = pci_config_read8(bus, slot, func, 0x08);
    dev->prog_if = pci_config_read8(bus, slot, func, 0x09);
    dev->subclass = pci_config_read8(bus, slot, func, 0x0A);
    dev->class_code = pci_config_read8(bus, slot, func, 0x0B);
    dev->header_type = pci_config_read8(bus, slot, func, 0x0E);

    if ((dev->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
        }
    }

    print_device(dev);
}

void pci_enumerate(void) {
    device_count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor_id = pci_config_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor_id == 0xFFFF) {
                continue;
            }

            uint8_t header_type = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                pci_probe_function((uint8_t)bus, slot, func);
            }
        }
    }

    kprintf("PCI: %d device(s) found\n", device_count);
}

const struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    return pci_find_device_nth(vendor_id, device_id, 0);
}

const struct pci_device *pci_find_device_nth(uint16_t vendor_id, uint16_t device_id, int index) {
    int seen = 0;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) {
            if (seen == index) {
                return &devices[i];
            }
            seen++;
        }
    }
    return NULL;
}

void pci_print_devices(void) {
    for (int i = 0; i < device_count; i++) {
        print_device(&devices[i]);
    }
    kprintf("PCI: %d device(s)\n", device_count);
}
