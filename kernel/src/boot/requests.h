#ifndef BOOT_REQUESTS_H
#define BOOT_REQUESTS_H

#include "limine.h"

#include <stdint.h>

/* All requests we make of the Limine bootloader, gathered in one place. */
/* See kernel/src/boot/requests.c for the definitions and PROTOCOL.md */
/* (vendored in limine/, or upstream at limine-bootloader/limine-protocol) */
/* for what each one means. */

extern volatile uint64_t limine_base_revision[3];
extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_rsdp_request rsdp_request;

#endif
