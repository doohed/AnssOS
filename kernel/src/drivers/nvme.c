#include "nvme.h"
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

/* NVM Express base specification (rev 1.4): controller registers in
 * section 3.1, queues and commands in 4 and 5, I/O commands in the NVM
 * command set. */

#define MAX_CONTROLLERS 4
#define QUEUE_ENTRIES 64 /* One page of 64-byte submission entries. */
#define BOUNCE_BYTES (64 * 1024)
#define COMMAND_TIMEOUT_MS 3000

#define REG_CAP 0x00
#define REG_VS 0x08
#define REG_INTMS 0x0C
#define REG_CC 0x14
#define REG_CSTS 0x1C
#define REG_AQA 0x24
#define REG_ASQ 0x28
#define REG_ACQ 0x30

#define CC_ENABLE 1u
#define CC_IOSQES (6u << 16) /* 64-byte submission entries */
#define CC_IOCQES (4u << 20) /* 16-byte completion entries */
#define CSTS_READY 1u
#define CSTS_FATAL 2u

#define ADMIN_CREATE_SQ 0x01
#define ADMIN_CREATE_CQ 0x05
#define ADMIN_IDENTIFY 0x06
#define IO_WRITE 0x01
#define IO_READ 0x02

struct sq_entry {
    uint32_t cdw0; /* Opcode, command ID in 31:16. */
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cq_entry {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head, sq_id;
    uint16_t command_id;
    uint16_t status; /* Phase tag in bit 0. */
};

struct queue {
    struct sq_entry *sq;
    struct cq_entry *cq;
    uint64_t sq_phys, cq_phys;
    uint32_t sq_tail, cq_head, phase;
    volatile uint32_t *sq_doorbell, *cq_doorbell;
};

struct nvme {
    volatile uint8_t *regs;
    struct queue admin, io;
    uint16_t next_id;
    uint32_t nsid;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint64_t *prp_list; /* For transfers over two pages. */
    uint64_t prp_list_phys;
    struct block_device block;
};

static struct nvme controllers[MAX_CONTROLLERS];
static int controller_count;

static uint32_t rd32(struct nvme *c, uint32_t off) {
    return *(volatile uint32_t *)(c->regs + off);
}
static uint64_t rd64(struct nvme *c, uint32_t off) {
    return rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32);
}
static void wr32(struct nvme *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->regs + off) = v;
}
static void wr64(struct nvme *c, uint32_t off, uint64_t v) {
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
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

static int wait_ready(struct nvme *c, uint32_t want, uint32_t timeout_ms) {
    for (uint32_t t = 0; t <= timeout_ms; t += 10) {
        uint32_t csts = rd32(c, REG_CSTS);
        if (csts == 0xFFFFFFFFu) {
            return -1; /* The device fell off the bus. */
        }
        if ((csts & CSTS_READY) == want) {
            return 0;
        }
        timer_sleep_ms(10);
    }
    return -1;
}

/* Submits one command and polls its completion. Returns the NVMe status
 * (0 = success) or -1 on timeout. */
static int submit(struct nvme *c, struct queue *q, struct sq_entry *cmd) {
    uint16_t id = c->next_id++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)id << 16);
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % QUEUE_ENTRIES;
    asm volatile("mfence" ::: "memory");
    *q->sq_doorbell = q->sq_tail;

    uint64_t deadline = timer_uptime_ms() + COMMAND_TIMEOUT_MS;
    for (uint32_t spins = 0;; spins++) {
        volatile struct cq_entry *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->phase) {
            uint16_t status = e->status >> 1;
            uint16_t got = e->command_id;
            q->cq_head = (q->cq_head + 1) % QUEUE_ENTRIES;
            if (q->cq_head == 0) {
                q->phase ^= 1;
            }
            *q->cq_doorbell = q->cq_head;
            if (got == id) {
                return status;
            }
            continue; /* A stale completion; keep looking for ours. */
        }
        if (spins > 100000) {
            if (timer_uptime_ms() > deadline) {
                return -1;
            }
            timer_idle();
        }
    }
}

static int queue_init(struct nvme *c, struct queue *q, uint32_t qid, uint32_t stride) {
    q->sq = dma_pages(1, &q->sq_phys);
    q->cq = dma_pages(1, &q->cq_phys);
    if (q->sq == NULL || q->cq == NULL) {
        return -1;
    }
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->sq_doorbell = (volatile uint32_t *)(c->regs + 0x1000 + (2 * qid) * stride);
    q->cq_doorbell = (volatile uint32_t *)(c->regs + 0x1000 + (2 * qid + 1) * stride);
    return 0;
}

/* Copies an Identify string field: space-padded, not NUL-terminated. */
static void copy_trimmed(char *out, const uint8_t *field, int len) {
    int n = len;
    while (n > 0 && (field[n - 1] == ' ' || field[n - 1] == 0)) {
        n--;
    }
    int start = 0;
    while (start < n && field[start] == ' ') {
        start++;
    }
    memcpy(out, field + start, (size_t)(n - start));
    out[n - start] = '\0';
}

/* Points the command at the bounce buffer: PRP1 is its first page, PRP2
 * the second page or, past two pages, a list of the rest (4.3). */
static void set_prps(struct nvme *c, struct sq_entry *cmd, uint32_t bytes) {
    uint32_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    cmd->prp1 = c->bounce_phys;
    if (pages == 2) {
        cmd->prp2 = c->bounce_phys + PMM_PAGE_SIZE;
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++) {
            c->prp_list[i - 1] = c->bounce_phys + (uint64_t)i * PMM_PAGE_SIZE;
        }
        cmd->prp2 = c->prp_list_phys;
    }
}

static int io(struct block_device *dev, int opcode, uint64_t lba, uint32_t count) {
    struct nvme *c = dev->driver;
    struct sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = (uint32_t)opcode;
    cmd.nsid = c->nsid;
    set_prps(c, &cmd, count * dev->sector_size);
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;
    int status = submit(c, &c->io, &cmd);
    if (status != 0) {
        kprintf("nvme: %s of %u sectors at %lu failed (status 0x%x)\n",
                opcode == IO_READ ? "read" : "write", count, lba, status);
        return -1;
    }
    return 0;
}

static int nvme_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct nvme *c = dev->driver;
    if (io(dev, IO_READ, lba, count) != 0) {
        return -1;
    }
    memcpy(buf, c->bounce, (size_t)count * dev->sector_size);
    return 0;
}

static int nvme_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct nvme *c = dev->driver;
    memcpy(c->bounce, buf, (size_t)count * dev->sector_size);
    return io(dev, IO_WRITE, lba, count);
}

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid) {
    struct sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = c->bounce_phys;
    cmd.cdw10 = cns;
    return submit(c, &c->admin, &cmd);
}

static int start(struct nvme *c, const struct pci_device *pci) {
    uint16_t pcicmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, (uint16_t)(pcicmd | 0x0406));
    uint64_t bar = pci->bar[0] & ~0xFull;
    if ((pci->bar[0] & 0x6) == 0x4) {
        bar |= (uint64_t)pci->bar[1] << 32;
    }
    c->regs = (volatile uint8_t *)vmm_map_mmio(bar, 0x2000);

    uint64_t cap = rd64(c, REG_CAP);
    uint32_t timeout_ms = (uint32_t)((cap >> 24) & 0xFF) * 500 + 500;
    uint32_t stride = 4u << ((cap >> 32) & 0xF);
    if (((cap >> 48) & 0xF) != 0) {
        kprintf("nvme: controller can't use 4 KiB pages\n");
        return -1;
    }

    /* Disable (a controller reset -- it touches no data), set up the
     * admin queues, enable. */
    wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_ENABLE);
    if (wait_ready(c, 0, timeout_ms) != 0) {
        kprintf("nvme: controller won't disable\n");
        return -1;
    }
    if (queue_init(c, &c->admin, 0, stride) != 0) {
        return -1;
    }
    wr32(c, REG_AQA, ((QUEUE_ENTRIES - 1) << 16) | (QUEUE_ENTRIES - 1));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    wr32(c, REG_INTMS, 0xFFFFFFFFu); /* Polled. */
    wr32(c, REG_CC, CC_IOSQES | CC_IOCQES | CC_ENABLE);
    if (wait_ready(c, 1, timeout_ms) != 0 || (rd32(c, REG_CSTS) & CSTS_FATAL)) {
        kprintf("nvme: controller won't enable\n");
        return -1;
    }

    c->bounce = dma_pages(BOUNCE_BYTES / PMM_PAGE_SIZE, &c->bounce_phys);
    c->prp_list = dma_pages(1, &c->prp_list_phys);
    if (c->bounce == NULL || c->prp_list == NULL) {
        return -1;
    }

    if (identify(c, 1, 0) != 0) { /* The controller */
        kprintf("nvme: Identify Controller failed\n");
        return -1;
    }
    copy_trimmed(c->block.model, c->bounce + 24, 40);
    uint8_t mdts = c->bounce[77];
    uint32_t max_bytes = BOUNCE_BYTES;
    if (mdts != 0 && mdts < 16 && ((uint32_t)PMM_PAGE_SIZE << mdts) < max_bytes) {
        max_bytes = (uint32_t)PMM_PAGE_SIZE << mdts;
    }

    c->nsid = 1;
    if (identify(c, 0, c->nsid) != 0) { /* Namespace 1 */
        kprintf("nvme: Identify Namespace failed\n");
        return -1;
    }
    uint64_t sectors;
    memcpy(&sectors, c->bounce, 8);
    uint8_t format = c->bounce[26] & 0xF;
    uint32_t lbads = c->bounce[128 + format * 4 + 2];
    if (sectors == 0 || lbads < 9 || lbads > 12) {
        kprintf("nvme: namespace 1 is unusable (%lu sectors, 2^%u bytes each)\n", sectors,
                lbads);
        return -1;
    }

    /* One I/O queue pair. */
    if (queue_init(c, &c->io, 1, stride) != 0) {
        return -1;
    }
    struct sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADMIN_CREATE_CQ;
    cmd.prp1 = c->io.cq_phys;
    cmd.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.cdw11 = 1; /* Physically contiguous, interrupts off. */
    if (submit(c, &c->admin, &cmd) != 0) {
        kprintf("nvme: creating the I/O completion queue failed\n");
        return -1;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADMIN_CREATE_SQ;
    cmd.prp1 = c->io.sq_phys;
    cmd.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.cdw11 = (1u << 16) | 1; /* Completes to CQ 1, physically contiguous. */
    if (submit(c, &c->admin, &cmd) != 0) {
        kprintf("nvme: creating the I/O submission queue failed\n");
        return -1;
    }

    c->block.sector_size = 1u << lbads;
    c->block.sector_count = sectors;
    c->block.max_sectors = max_bytes / c->block.sector_size;
    c->block.read = nvme_read;
    c->block.write = nvme_write;
    c->block.driver = c;
    return 0;
}

int nvme_init(void) {
    for (int i = 0; controller_count < MAX_CONTROLLERS; i++) {
        const struct pci_device *pci = pci_find_class_nth(0x01, 0x08, 0x02, i);
        if (pci == NULL) {
            break;
        }
        struct nvme *c = &controllers[controller_count];
        memset(c, 0, sizeof(*c));
        kprintf("nvme: controller at %u:%u.%u (%x:%x)\n", pci->bus, pci->slot, pci->func,
                pci->vendor_id, pci->device_id);
        if (start(c, pci) != 0) {
            continue;
        }
        c->block.name[0] = 'n';
        c->block.name[1] = 'v';
        c->block.name[2] = 'm';
        c->block.name[3] = 'e';
        c->block.name[4] = (char)('0' + controller_count);
        c->block.name[5] = '\0';
        block_register(&c->block);
        controller_count++;
    }
    return controller_count;
}
