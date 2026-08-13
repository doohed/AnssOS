#include "process.h"
#include "elf.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/usermode.h"
#include "../drivers/serial.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

static struct process processes[MAX_PROCESSES];
static int next_pid = 1;
static struct process *current_process;

static struct process *alloc_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            return &processes[i];
        }
    }
    return NULL;
}

struct process *process_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROC_UNUSED && processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

struct process *process_current(void) {
    return current_process;
}

struct process *process_find_child(int parent_pid, int pid) {
    if (pid >= 0) {
        struct process *p = process_by_pid(pid);
        return (p != NULL && p->parent_pid == parent_pid) ? p : NULL;
    }

    struct process *any = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED || processes[i].parent_pid != parent_pid) {
            continue;
        }
        if (processes[i].state == PROC_ZOMBIE) {
            return &processes[i];
        }
        if (any == NULL) {
            any = &processes[i];
        }
    }
    return any;
}

int process_spawn(const uint8_t *image, size_t image_size, int parent_pid) {
    struct process *p = alloc_slot();
    if (p == NULL) {
        kprintf("process: too many processes (max %d)\n", MAX_PROCESSES);
        return -1;
    }

    if (elf_load(image, image_size, &p->task) != 0) {
        return -1;
    }

    p->pid = next_pid++;
    p->parent_pid = parent_pid;
    p->state = PROC_RUNNABLE;
    p->exit_status = 0;
    p->has_run = 0;
    p->saved_rsp = 0;
    return p->pid;
}

int process_fork(struct process *parent, struct interrupt_frame *frame) {
    struct process *child = alloc_slot();
    if (child == NULL) {
        return -1;
    }

    /* Shallow-copies heap range/open_files/cwd as-is (sharing the same
     * vnode pointers -- an independent copy of each file's *offset*,
     * not the Unix-real "shared open file description" a real fork()
     * gives, a deliberate simplification); `as` gets replaced below with
     * a real clone, not the parent's own. */
    child->task = parent->task;

    child->task.as = vmm_new_address_space();
    if (vmm_clone_user_pages(&child->task.as, &parent->task.as) != 0) {
        child->state = PROC_UNUSED;
        return -1;
    }

    uint8_t *kstack = kmalloc(ELF_KERNEL_STACK_SIZE);
    if (kstack == NULL) {
        child->state = PROC_UNUSED;
        return -1;
    }
    uint64_t kstack_top = (uint64_t)(uintptr_t)(kstack + ELF_KERNEL_STACK_SIZE);
    child->task.kernel_stack_top = kstack_top;

    /* Build the child's initial saved frame at the top of its own fresh
     * kernel stack: an exact copy of the parent's own syscall frame,
     * except rax=0 -- see process.h's own doc comment on why. */
    struct interrupt_frame *child_frame =
        (struct interrupt_frame *)(kstack_top - sizeof(struct interrupt_frame));
    *child_frame = *frame;
    child_frame->rax = 0;

    child->pid = next_pid++;
    child->parent_pid = parent->pid;
    child->state = PROC_RUNNABLE;
    child->exit_status = 0;
    child->has_run = 1;
    child->saved_rsp = (uint64_t)child_frame;

    return child->pid;
}

int process_exec(struct process *p, const uint8_t *image, size_t image_size) {
    struct open_file saved_files[MAX_OPEN_FILES];
    memcpy(saved_files, p->task.open_files, sizeof(saved_files));
    struct vnode *saved_cwd = p->task.cwd;
    /* p->task.kernel_resume is *live* right now -- it's what the
     * return_to_kernel() call the caller (sys_execve_impl) makes right
     * after this returns will read from, to correctly unwind back to
     * whichever dispatch call launched *this* instance of `p`. A plain
     * `p->task = new_task` below would zero it (elf_load() memsets the
     * whole struct), stranding that pending unwind -- save/restore it
     * exactly like open_files/cwd, even though the *next* dispatch
     * (has_run reset to 0 below) will overwrite it again regardless. */
    uint64_t saved_kernel_resume[7];
    memcpy(saved_kernel_resume, p->task.kernel_resume, sizeof(saved_kernel_resume));

    struct usertask new_task;
    if (elf_load(image, image_size, &new_task) != 0) {
        return -1;
    }

    /* The old address space/kernel stack are simply abandoned here (no
     * teardown) -- same accepted leak as everywhere else this project
     * doesn't free page-table memory back to the PMM yet. */
    p->task = new_task;
    memcpy(p->task.open_files, saved_files, sizeof(saved_files));
    p->task.cwd = saved_cwd;
    memcpy(p->task.kernel_resume, saved_kernel_resume, sizeof(saved_kernel_resume));
    p->has_run = 0;           /* Next dispatch is a fresh launch at the new entry. */
    p->state = PROC_RUNNABLE; /* Was PROC_RUNNING; the caller is about to abandon this
                               * dispatch via return_to_kernel() -- without this, the
                               * scheduler would never pick it again. */
    return 0;
}

static struct process *pick_next_runnable(void) {
    static int last_picked = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        int idx = (last_picked + 1 + i) % MAX_PROCESSES;
        if (processes[idx].state == PROC_RUNNABLE) {
            last_picked = idx;
            return &processes[idx];
        }
    }
    return NULL;
}

int scheduler_run_until(int wait_pid) {
    struct process *outer_current = current_process;
    int result;

    for (;;) {
        if (wait_pid >= 0) {
            struct process *target = process_by_pid(wait_pid);
            if (target == NULL) {
                result = -1;
                goto done;
            }
            if (target->state == PROC_ZOMBIE) {
                result = target->exit_status;
                target->state = PROC_UNUSED;
                goto done;
            }
        } else {
            int any_alive = 0;
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state == PROC_ZOMBIE) {
                    /* No init process to reparent orphans to -- see
                     * process.h's doc comment: the convention is that a
                     * process which fork()s a child waits for it before
                     * exiting, so the only zombie a -1 caller should
                     * ever actually witness is its own top-level launch. */
                    processes[i].state = PROC_UNUSED;
                    continue;
                }
                if (processes[i].state != PROC_UNUSED) {
                    any_alive = 1;
                }
            }
            if (!any_alive) {
                result = 0;
                goto done;
            }
        }

        struct process *next = pick_next_runnable();
        if (next == NULL) {
            /* Nothing runnable right now (Phase A: shouldn't normally
             * happen -- a fully cooperative model with no I/O blocking
             * yet has no way to reach this except a bug). Wait for an
             * interrupt rather than spin. */
            asm volatile("sti; hlt");
            continue;
        }

        next->state = PROC_RUNNING;
        current_process = next;
        int exit_status;
        if (!next->has_run) {
            next->has_run = 1;
            enter_usermode(&next->task, &exit_status);
        } else {
            resume_usermode(&next->task, next->saved_rsp, &exit_status);
        }
        /* `next`'s own state was already updated (PROC_ZOMBIE,
         * PROC_RUNNABLE after exec()/preemption) by whichever syscall/
         * interrupt path caused this to return, before it called
         * return_to_kernel() -- the top-of-loop checks above notice it
         * next iteration. */
    }

done:
    current_process = outer_current;
    /* TSS.RSP0 was left pointing at whichever process most recently ran
     * (possibly several dispatches deep, e.g. wait()'s own recursive
     * call into this same function) -- restore it to the process this
     * call is unwinding back into (if any; NULL means back to the
     * top-level shell/kernel context, which never takes a ring-3-
     * originated interrupt and so doesn't need TSS.RSP0 to mean
     * anything). Without this, the *outer* process's own next syscall/
     * fault builds its interrupt frame on the *wrong* kernel stack. */
    if (outer_current != NULL) {
        tss_set_kernel_stack(outer_current->task.kernel_stack_top);
    }
    return result;
}
