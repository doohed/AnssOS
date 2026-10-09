#include "acpi.h"
#include "serial.h"
#include "../arch/x86_64/io.h"
#include "../boot/requests.h"
#include "../lib/string.h"
#include "../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

struct __attribute__((packed)) acpi_rsdp {
    char signature[8]; /* "RSD PTR " */
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision; /* 0 = ACPI 1.0 (RSDT only), 2+ = has the XSDT. */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct __attribute__((packed)) acpi_header {
    char signature[4];
    uint32_t length; /* Including this header. */
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* FADT field offsets used here (ACPI spec, "Fixed ACPI Description
 * Table"); the table is long and only these matter. */
#define FADT_PM_TMR_BLK 76    /* u32 I/O port */
#define FADT_FLAGS 112        /* u32 */
#define FADT_X_PM_TMR_BLK 208 /* 12-byte Generic Address Structure */
#define FADT_FLAG_TMR_VAL_EXT (1u << 8) /* PM timer is 32 bits, not 24. */
#define GAS_SPACE_IO 1

static uint16_t pm_port;
static uint32_t pm_mask;

/* ACPI tables can sit in memory the HHDM doesn't cover (firmware-reserved
 * ranges), so each one gets its own mapping: the header first, to learn
 * the length, then the whole table. */
static const struct acpi_header *map_table(uint64_t phys) {
    const struct acpi_header *h =
        (const struct acpi_header *)vmm_map_mmio(phys, sizeof(struct acpi_header));
    uint32_t len = h->length;
    if (len < sizeof(struct acpi_header) || len > 0x100000) {
        return NULL;
    }
    return (const struct acpi_header *)vmm_map_mmio(phys, len);
}

static uint32_t read_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void parse_fadt(const struct acpi_header *fadt) {
    const uint8_t *f = (const uint8_t *)fadt;
    uint32_t len = fadt->length;

    uint64_t port = 0;
    if (len >= FADT_X_PM_TMR_BLK + 12 && f[FADT_X_PM_TMR_BLK] == GAS_SPACE_IO) {
        port = read_u64(f + FADT_X_PM_TMR_BLK + 4);
    }
    if (port == 0 && len >= FADT_PM_TMR_BLK + 4) {
        port = read_u32(f + FADT_PM_TMR_BLK);
    }
    if (port == 0 || port > 0xFFFF) {
        kprintf("acpi: FADT has no I/O-port PM timer\n");
        return;
    }
    pm_port = (uint16_t)port;
    int ext = len >= FADT_FLAGS + 4 && (read_u32(f + FADT_FLAGS) & FADT_FLAG_TMR_VAL_EXT);
    pm_mask = ext ? 0xFFFFFFFFu : 0x00FFFFFFu;
}

int acpi_init(void) {
    if (rsdp_request.response == NULL || rsdp_request.response->address == NULL) {
        kprintf("acpi: no RSDP from the bootloader\n");
        return -1;
    }
    /* Limine hands over an HHDM pointer here; turn it back into a
     * physical address so every table goes through the same mapping. */
    uint64_t rsdp_addr = (uint64_t)(uintptr_t)rsdp_request.response->address;
    uint64_t hhdm = hhdm_request.response->offset;
    uint64_t rsdp_phys = rsdp_addr >= hhdm ? rsdp_addr - hhdm : rsdp_addr;
    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)vmm_map_mmio(rsdp_phys, sizeof(struct acpi_rsdp));
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        kprintf("acpi: bad RSDP signature\n");
        return -1;
    }

    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const struct acpi_header *root =
        map_table(use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    if (root == NULL) {
        kprintf("acpi: unreadable %s\n", use_xsdt ? "XSDT" : "RSDT");
        return -1;
    }

    uint32_t entry_size = use_xsdt ? 8 : 4;
    uint32_t count = (root->length - sizeof(struct acpi_header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(struct acpi_header);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = entries + i * entry_size;
        uint64_t phys = use_xsdt ? read_u64(e) : read_u32(e);
        const struct acpi_header *h = map_table(phys);
        if (h != NULL && memcmp(h->signature, "FACP", 4) == 0) {
            parse_fadt(h);
            kprintf("acpi: %s with %u tables; PM timer %s\n", use_xsdt ? "XSDT" : "RSDT", count,
                    pm_port ? "found" : "missing");
            return 0;
        }
    }
    kprintf("acpi: no FADT among %u tables\n", count);
    return -1;
}

int acpi_pm_timer_present(void) {
    return pm_port != 0;
}

uint32_t acpi_pm_timer_read(void) {
    return inl(pm_port) & pm_mask;
}

uint32_t acpi_pm_timer_delta(uint32_t from, uint32_t to) {
    return (to - from) & pm_mask;
}

uint16_t acpi_pm_timer_port(void) {
    return pm_port;
}

int acpi_pm_timer_bits(void) {
    return pm_mask == 0xFFFFFFFFu ? 32 : 24;
}
