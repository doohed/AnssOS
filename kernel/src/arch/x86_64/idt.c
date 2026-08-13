#include "idt.h"
#include "pic.h"
#include "usermode.h"
#include "../../drivers/serial.h"
#include "../../exec/process.h"

#include <stddef.h>
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
/* defined in isr.S. irq_stub_table[0..15]: same idea for hardware IRQs */
/* (vectors 32-47), defined in irq.S. isr_stub_syscall: the vector-0x80 */
/* syscall trampoline, defined in syscall.S. */
extern void *isr_stub_table[32];
extern void *irq_stub_table[16];
extern void isr_stub_syscall(void);

#define VECTOR_SYSCALL 0x80

static void (*irq_handlers[16])(void);

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
        "Divide-by-zero",
        "Debug",
        "NMI",
        "Breakpoint",
        "Overflow",
        "Bound Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Reserved",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 FPU Error",
        "Alignment Check",
        "Machine Check",
        "SIMD FP Exception",
        "Virtualization Exception",
        "Control Protection Exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor Injection",
        "VMM Communication",
        "Security Exception",
        "Reserved",
    };
    return vector < 32 ? names[vector] : "Unknown";
}

/* Called from isr_common_stub with rdi pointing at the saved register/
 * frame state. A fault taken from ring 3 (frame->cs & 3 == 3 -- can only
 * happen once a user task is actually running, via enter_usermode())
 * doesn't bring down the kernel: it ends *that* process (marked
 * PROC_ZOMBIE, same as an explicit exit syscall -- see exec/syscall.c's
 * SYS_exit -- so the scheduler correctly reaps it and a top-level
 * scheduler_run_until(-1) caller, e.g. the shell's `run`, doesn't hang
 * waiting for a slot that will never become a zombie any other way) and
 * returns control to the scheduler. A fault from ring 0 remains fatal (a
 * genuine kernel bug, not recoverable until we have real fault handling,
 * e.g. page fault -> demand paging). */
void isr_handler(struct interrupt_frame *frame) {
    if ((frame->cs & 3) == 3) {
        kprintf("\nuser program crashed: %s (vector %lu, error 0x%lx) at rip=0x%lx\n",
                exception_name(frame->vector), frame->vector, frame->error_code, frame->rip);
        struct process *me = process_current();
        if (me != NULL) {
            me->state = PROC_ZOMBIE;
            me->exit_status = -1;
        }
        return_to_kernel(-1);
    }

    kprintf("\n--- CPU EXCEPTION: %s (vector %lu, error 0x%lx) ---\n",
            exception_name(frame->vector), frame->vector, frame->error_code);
    kprintf("rip=0x%lx cs=0x%lx rflags=0x%lx rsp=0x%lx ss=0x%lx\n", frame->rip, frame->cs,
            frame->rflags, frame->rsp, frame->ss);
    kprintf("rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n", frame->rax, frame->rbx, frame->rcx,
            frame->rdx);
    kprintf("rsi=0x%lx rdi=0x%lx rbp=0x%lx\n", frame->rsi, frame->rdi, frame->rbp);
    kprintf("r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx\n", frame->r8, frame->r9, frame->r10,
            frame->r11);
    kprintf("r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n", frame->r12, frame->r13, frame->r14,
            frame->r15);

    for (;;) {
        asm volatile("cli; hlt");
    }
}

void irq_register(uint8_t irq, void (*handler)(void)) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

/* Called from irq_common_stub. frame->vector is 32+irq (see pic.h) --
 * dispatch to whatever driver registered that line, then EOI. A handler
 * with nothing registered (including genuinely spurious IRQs) is just a
 * silent EOI, not an error.
 *
 * M13 Phase B preemption: if this was the timer (IRQ0) interrupting
 * ring-3 code, hand the CPU to a different runnable process instead of
 * just returning to the one that was running. This happens *after* the
 * normal dispatch/EOI above -- pit.c's own registered handler still
 * ticks pit_ticks() every time regardless of whether a preemption
 * follows, and EOI must happen before we potentially abandon this
 * interrupt entirely (skipping it would leave the PIC thinking IRQ0 is
 * still in service, blocking further timer interrupts). The process
 * being preempted isn't "done" the way exit()/a crash is -- its full
 * register state is already sitting on its own kernel stack exactly
 * where `frame` points (see usermode.S's arch_resume_process), so
 * resuming it later needs nothing more than remembering that address
 * and marking it PROC_RUNNABLE again. */
void irq_handler(struct interrupt_frame *frame) {
    uint64_t irq = frame->vector - PIC_IRQ_BASE;
    if (irq < 16 && irq_handlers[irq] != NULL) {
        irq_handlers[irq]();
    }
    pic_send_eoi((uint8_t)irq);

    if (frame->vector == PIC_IRQ_BASE && (frame->cs & 3) == 3) {
        struct process *me = process_current();
        if (me != NULL) {
            me->saved_rsp = (uint64_t)frame;
            me->state = PROC_RUNNABLE;
            return_to_kernel(0); /* noreturn -- lands back in the scheduler loop */
        }
    }
}

void idt_init(void) {
    for (int vector = 0; vector < 32; vector++) {
        /* present, DPL0, 64-bit interrupt gate (0x8E); IST unused (0). */
        idt_set_gate(vector, isr_stub_table[vector], 0x8E);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_gate(PIC_IRQ_BASE + i, irq_stub_table[i], 0x8E);
    }

    /* present, DPL3 (callable via `int 0x80` from ring 3), 64-bit
     * interrupt gate (0xEE) -- the one deliberately non-DPL0 gate in the
     * table. */
    idt_set_gate(VECTOR_SYSCALL, isr_stub_syscall, 0xEE);

    idtr_val.limit = sizeof(idt) - 1;
    idtr_val.base = (uint64_t)&idt;
    asm volatile("lidt (%0)" : : "r"(&idtr_val) : "memory");
}
