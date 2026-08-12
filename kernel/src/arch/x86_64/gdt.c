#include "gdt.h"
#include "../../lib/string.h"

#include <stdint.h>

/* Selectors, fixed by the layout of struct gdt_table below. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_TSS   0x28

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

/* A TSS descriptor is a 16-byte "system" descriptor -- twice the size of */
/* a normal code/data descriptor -- because it needs a full 64-bit base. */
struct __attribute__((packed)) tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
};

struct __attribute__((packed)) gdt_table {
    struct gdt_entry null;
    struct gdt_entry kcode;
    struct gdt_entry kdata;
    struct gdt_entry ucode;
    struct gdt_entry udata;
    struct tss_descriptor tss;
};

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

static struct gdt_table gdt;
static struct tss tss;
static struct gdtr gdtr_val;

static void gdt_set_entry(struct gdt_entry *e, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity_flags) {
    e->limit_low = limit & 0xFFFF;
    e->base_low = base & 0xFFFF;
    e->base_mid = (base >> 16) & 0xFF;
    e->access = access;
    e->granularity = ((limit >> 16) & 0x0F) | (granularity_flags & 0xF0);
    e->base_high = (base >> 24) & 0xFF;
}

static void gdt_set_tss_descriptor(struct tss_descriptor *d, uint64_t base, uint32_t limit) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->access = 0x89; /* present, DPL0, type 0x9 = 64-bit TSS (available) */
    d->granularity = (limit >> 16) & 0x0F;
    d->base_high = (base >> 24) & 0xFF;
    d->base_upper = (uint32_t)(base >> 32);
    d->reserved = 0;
}

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)

/* Reload CS via a far return -- in long mode you can't just `mov %cs`. */
/* Selectors are embedded as literal immediates at compile time so there's */
/* no ambiguity around how GCC would prefix an "i"-constrained operand. */
static void gdt_reload_segments(void) {
    asm volatile (
        "mov $" STRINGIFY(SEL_KDATA) ", %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "pushq $" STRINGIFY(SEL_KCODE) "\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        :
        : "rax", "memory"
    );
}

void gdt_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss); /* No I/O permission bitmap. */

    gdt_set_entry(&gdt.null, 0, 0, 0, 0);
    gdt_set_entry(&gdt.kcode, 0, 0xFFFFF, 0x9A, 0xA0); /* P,DPL0,code; L=1 */
    gdt_set_entry(&gdt.kdata, 0, 0xFFFFF, 0x92, 0xC0); /* P,DPL0,data */
    gdt_set_entry(&gdt.ucode, 0, 0xFFFFF, 0xFA, 0xA0); /* P,DPL3,code; L=1 */
    gdt_set_entry(&gdt.udata, 0, 0xFFFFF, 0xF2, 0xC0); /* P,DPL3,data */
    gdt_set_tss_descriptor(&gdt.tss, (uint64_t)&tss, sizeof(tss) - 1);

    gdtr_val.limit = sizeof(gdt) - 1;
    gdtr_val.base = (uint64_t)&gdt;

    asm volatile ("lgdt (%0)" : : "r"(&gdtr_val) : "memory");
    gdt_reload_segments();
    asm volatile ("ltr %%ax" : : "a"((uint16_t)SEL_TSS));
}
