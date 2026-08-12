#include "idt.h"
#include "../../drivers/serial.h"

#include <stdint.h>

#define IDT_ENTRIES 256
#define SEL_KCODE 0x08

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[IDT_ENTRIES];
static struct idtr idtr_val;

/* isr_stub_table[0..31]: one small asm trampoline per CPU exception vector, */
/* defined in isr.S. */
extern void *isr_stub_table[32];

static void idt_set_gate(int vector, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low = addr & 0xFFFF;
    idt[vector].selector = SEL_KCODE;
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

static const char *exception_name(uint64_t vector) {
    static const char *names[32] = {
        "Divide-by-zero", "Debug", "NMI", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Reserved", "Invalid TSS", "Segment Not Present",
        "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP Exception",
        "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved",
    };
    return vector < 32 ? names[vector] : "Unknown";
}

/* Called from isr_common_stub with rdi pointing at the saved register/ */
/* frame state. There's no recovery path yet -- any CPU exception is fatal */
/* until we have real fault handling (e.g. page fault -> demand paging). */
void isr_handler(struct interrupt_frame *frame) {
    kprintf("\n--- CPU EXCEPTION: %s (vector %lu, error 0x%lx) ---\n",
            exception_name(frame->vector), frame->vector, frame->error_code);
    kprintf("rip=0x%lx cs=0x%lx rflags=0x%lx rsp=0x%lx ss=0x%lx\n",
            frame->rip, frame->cs, frame->rflags, frame->rsp, frame->ss);
    kprintf("rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    kprintf("rsi=0x%lx rdi=0x%lx rbp=0x%lx\n", frame->rsi, frame->rdi, frame->rbp);
    kprintf("r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    kprintf("r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
            frame->r12, frame->r13, frame->r14, frame->r15);

    for (;;) {
        asm volatile ("cli; hlt");
    }
}

void idt_init(void) {
    for (int vector = 0; vector < 32; vector++) {
        /* present, DPL0, 64-bit interrupt gate (0x8E); IST unused (0). */
        idt_set_gate(vector, isr_stub_table[vector], 0x8E);
    }

    idtr_val.limit = sizeof(idt) - 1;
    idtr_val.base = (uint64_t)&idt;
    asm volatile ("lidt (%0)" : : "r"(&idtr_val) : "memory");
}
