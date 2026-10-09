#include "ahci.h"
#include "block.h"
#include "pci.h"
#include "serial.h"
#include "timer.h"
#include "../boot/requests.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

/* Serial ATA AHCI 1.3.1 (HBA and port registers, section 3; command
 * lists and tables, 4) and ATA8-ACS (IDENTIFY DEVICE, READ/WRITE DMA EXT). */

#define MAX_DISKS 8
#define BOUNCE_BYTES (64 * 1024)
#define COMMAND_TIMEOUT_MS 5000

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI 0x0C
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define GHC_AHCI_ENABLE (1u << 31)

#define PORT(p) (0x100 + 0x80 * (p))
#define PX_CLB 0x00
#define PX_FB 0x08
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38

#define CMD_ST (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)
#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define IS_TFES (1u << 30)

#define SIG_ATA 0x00000101u
#define SIG_ATAPI 0xEB140101u

#define ATA_IDENTIFY 0xEC
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35

struct command_header {
    uint16_t flags; /* FIS length in dwords (4:0), write (6). */
    uint16_t prdt_length;
    volatile uint32_t bytes_done;
    uint64_t table;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint64_t address;
    uint32_t reserved;
    uint32_t count; /* Bytes - 1 in 21:0. */
};

struct command_table {
    uint8_t fis[64];
    uint8_t atapi[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[1];
};

struct ahci_disk {
    volatile uint8_t *port; /* This port's registers. */
    struct command_header *list;
    struct command_table *table;
    uint8_t *bounce;
    uint64_t bounce_phys;
    struct block_device block;
};

static struct ahci_disk disks[MAX_DISKS];
static int disk_count;

static uint32_t rd32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static int wait_clear(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t ms) {
    for (uint32_t t = 0; t <= ms; t++) {
        if (!(rd32(base, off) & mask)) {
            return 0;
        }
        timer_sleep_ms(1);
    }
    return -1;
}

static void *dma_pages(uint64_t count, uint64_t *phys) {
    *phys = count == 1 ? pmm_alloc_page() : pmm_alloc_pages(count);
    if (*phys == 0) {
        return NULL;
    }
    void *v = (void *)(uintptr_t)(*phys + hhdm_request.response->offset);
    memset(v, 0, count * PMM_PAGE_SIZE);
    return v;
}

/* Issues one command in slot 0 and waits for it. `bytes` of data move
 * through the bounce buffer. */
static int run(struct ahci_disk *d, uint8_t command, uint64_t lba, uint16_t count, uint32_t bytes,
               int write) {
    if (wait_clear(d->port, PX_TFD, TFD_BSY | TFD_DRQ, 1000) != 0) {
        kprintf("ahci: %s: drive stays busy\n", d->block.name);
        return -1;
    }
    uint8_t *f = d->table->fis;
    memset(f, 0, 20);
    f[0] = 0x27; /* Register FIS, host to device */
    f[1] = 0x80; /* ...carrying a command */
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40; /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);

    d->table->prdt[0].address = d->bounce_phys;
    d->table->prdt[0].count = bytes - 1;
    d->list[0].flags = 5 | (write ? (1u << 6) : 0); /* 5-dword FIS */
    d->list[0].prdt_length = 1;
    d->list[0].bytes_done = 0;

    wr32(d->port, PX_IS, 0xFFFFFFFFu);
    asm volatile("mfence" ::: "memory");
    wr32(d->port, PX_CI, 1);

    uint64_t deadline = timer_uptime_ms() + COMMAND_TIMEOUT_MS;
    for (uint32_t spins = 0;; spins++) {
        if (rd32(d->port, PX_IS) & IS_TFES) {
            kprintf("ahci: %s: command 0x%x failed (status 0x%x)\n", d->block.name, command,
                    rd32(d->port, PX_TFD));
            return -1;
        }
        if (!(rd32(d->port, PX_CI) & 1)) {
            break;
        }
        if (spins > 100000) {
            if (timer_uptime_ms() > deadline) {
                kprintf("ahci: %s: command 0x%x timed out\n", d->block.name, command);
                return -1;
            }
            timer_idle();
        }
    }
    return (rd32(d->port, PX_TFD) & TFD_ERR) ? -1 : 0;
}

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct ahci_disk *d = dev->driver;
    uint32_t bytes = count * dev->sector_size;
    if (run(d, ATA_READ_DMA_EXT, lba, (uint16_t)count, bytes, 0) != 0) {
        return -1;
    }
    memcpy(buf, d->bounce, bytes);
    return 0;
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct ahci_disk *d = dev->driver;
    uint32_t bytes = count * dev->sector_size;
    memcpy(d->bounce, buf, bytes);
    return run(d, ATA_WRITE_DMA_EXT, lba, (uint16_t)count, bytes, 1);
}

/* IDENTIFY strings are big-endian within each 16-bit word. */
static void ata_string(char *out, const uint16_t *words, int nwords) {
    int n = 0;
    for (int i = 0; i < nwords; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)(words[i] & 0xFF);
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) {
        n--;
    }
    out[n] = '\0';
}

/* Takes over one port with a drive on it: stop its engines, give it our
 * command list and FIS area, start again, then IDENTIFY. Nothing here
 * touches the drive's data. */
static int start_port(struct ahci_disk *d, volatile uint8_t *port, int index) {
    d->port = port;
    wr32(port, PX_CMD, rd32(port, PX_CMD) & ~CMD_ST);
    if (wait_clear(port, PX_CMD, CMD_CR, 500) != 0) {
        return -1;
    }
    wr32(port, PX_CMD, rd32(port, PX_CMD) & ~CMD_FRE);
    if (wait_clear(port, PX_CMD, CMD_FR, 500) != 0) {
        return -1;
    }

    uint64_t page_phys, table_phys;
    uint8_t *page = dma_pages(1, &page_phys); /* Command list at 0, received FISes at 1 KiB */
    d->table = dma_pages(1, &table_phys);
    d->bounce = dma_pages(BOUNCE_BYTES / PMM_PAGE_SIZE, &d->bounce_phys);
    if (page == NULL || d->table == NULL || d->bounce == NULL) {
        return -1;
    }
    d->list = (struct command_header *)page;
    d->list[0].table = table_phys;
    wr32(port, PX_CLB, (uint32_t)page_phys);
    wr32(port, PX_CLB + 4, (uint32_t)(page_phys >> 32));
    wr32(port, PX_FB, (uint32_t)(page_phys + 1024));
    wr32(port, PX_FB + 4, (uint32_t)((page_phys + 1024) >> 32));
    wr32(port, PX_SERR, 0xFFFFFFFFu);
    wr32(port, PX_IS, 0xFFFFFFFFu);
    wr32(port, PX_IE, 0); /* Polled. */
    wr32(port, PX_CMD, rd32(port, PX_CMD) | CMD_FRE);
    wr32(port, PX_CMD, rd32(port, PX_CMD) | CMD_ST);

    if (run(d, ATA_IDENTIFY, 0, 0, 512, 0) != 0) {
        kprintf("ahci: port %d: IDENTIFY failed\n", index);
        return -1;
    }
    const uint16_t *id = (const uint16_t *)d->bounce;
    if (!(id[83] & (1u << 10))) {
        kprintf("ahci: port %d: drive has no 48-bit addressing -- skipped\n", index);
        return -1;
    }
    uint64_t sectors = id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[103] << 48);
    uint32_t sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        sector_size = (id[117] | ((uint32_t)id[118] << 16)) * 2;
    }
    if (sectors == 0 || (sector_size != 512 && sector_size != 4096)) {
        kprintf("ahci: port %d: unusable geometry\n", index);
        return -1;
    }
    ata_string(d->block.model, id + 27, 20);
    d->block.sector_size = sector_size;
    d->block.sector_count = sectors;
    d->block.max_sectors = BOUNCE_BYTES / sector_size;
    d->block.read = ahci_read;
    d->block.write = ahci_write;
    d->block.driver = d;
    return 0;
}

/* AHCI's BIOS/OS handoff (10.6), when the controller supports it. */
static void bios_handoff(volatile uint8_t *hba) {
    if (!(rd32(hba, HBA_CAP2) & 1)) {
        return;
    }
    wr32(hba, HBA_BOHC, rd32(hba, HBA_BOHC) | (1u << 1)); /* OS owned */
    wait_clear(hba, HBA_BOHC, 1u << 0, 50);                /* BIOS owned */
    if (rd32(hba, HBA_BOHC) & (1u << 4)) {
        wait_clear(hba, HBA_BOHC, 1u << 4, 2000); /* BIOS busy */
    }
}

int ahci_init(void) {
    for (int i = 0;; i++) {
        const struct pci_device *pci = pci_find_class_nth(0x01, 0x06, 0x01, i);
        if (pci == NULL) {
            break;
        }
        uint16_t cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
        pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, (uint16_t)(cmd | 0x0406));
        volatile uint8_t *hba =
            (volatile uint8_t *)vmm_map_mmio(pci->bar[5] & ~0xFull, PORT(32));

        bios_handoff(hba);
        wr32(hba, HBA_GHC, rd32(hba, HBA_GHC) | GHC_AHCI_ENABLE);
        uint32_t implemented = rd32(hba, HBA_PI);
        kprintf("ahci: controller at %u:%u.%u (%x:%x)\n", pci->bus, pci->slot, pci->func,
                pci->vendor_id, pci->device_id);

        for (int p = 0; p < 32 && disk_count < MAX_DISKS; p++) {
            if (!(implemented & (1u << p))) {
                continue;
            }
            volatile uint8_t *port = hba + PORT(p);
            if ((rd32(port, PX_SSTS) & 0xF) != 3) {
                continue; /* Nothing attached. */
            }
            uint32_t sig = rd32(port, PX_SIG);
            if (sig != SIG_ATA) {
                kprintf("ahci: port %d: %s -- skipped\n", p,
                        sig == SIG_ATAPI ? "CD/DVD drive" : "not a disk");
                continue;
            }
            struct ahci_disk *d = &disks[disk_count];
            memset(d, 0, sizeof(*d));
            if (start_port(d, port, p) != 0) {
                continue;
            }
            d->block.name[0] = 's';
            d->block.name[1] = 'a';
            d->block.name[2] = 't';
            d->block.name[3] = 'a';
            d->block.name[4] = (char)('0' + disk_count);
            d->block.name[5] = '\0';
            block_register(&d->block);
            disk_count++;
        }
    }
    return disk_count;
}
