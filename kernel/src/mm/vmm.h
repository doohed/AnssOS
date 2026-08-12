#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>

/* Minimal virtual memory support: mapping arbitrary physical MMIO ranges */
/* (PCI BARs) into virtual memory. Everything else still rides on */
/* Limine's HHDM -- this exists only because PCI BARs (especially 64-bit */
/* ones, which QEMU/OVMF often place far above actual RAM, e.g. */
/* 0xc000000000) commonly live outside the range Limine's HHDM covers */
/* (HHDM only maps memory described in the memory map). There's no */
/* unmapping, no user address spaces, no demand paging here: this is just */
/* enough to get MMIO device registers reachable. */

/* Maps `size` bytes of physical memory starting at `phys_addr` (need not */
/* be page-aligned) as present + writable + uncacheable, walking/extending */
/* the page tables already active at boot (allocating any missing */
/* intermediate levels via the PMM). Returns a virtual pointer to the same */
/* byte offset within the mapping. Never fails -- if the PMM is out of */
/* memory for page tables, that's a fatal boot-time condition anyway. */
volatile void *vmm_map_mmio(uint64_t phys_addr, uint64_t size);

#endif
