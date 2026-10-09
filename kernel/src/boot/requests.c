#include "requests.h"

#include <stddef.h>

/* Every request struct (and the base revision marker) must live in the */
/* .limine_requests section, kept alive with "used" since nothing else */
/* references them, and bracketed by the start/end marker symbols below. */
/* See kernel/linker.ld for where that section gets placed. */

__attribute__((used, section(".limine_requests"))) volatile uint64_t limine_base_revision[3] =
    LIMINE_BASE_REVISION(6);

__attribute__((used,
               section(".limine_requests"))) volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((
    used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((
    used,
    section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used,
               section(".limine_requests"))) volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct
    limine_executable_cmdline_request cmdline_request = {
        .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
        .revision = 0,
};

int boot_option(const char *word) {
    if (cmdline_request.response == NULL || cmdline_request.response->cmdline == NULL) {
        return 0;
    }
    const char *p = cmdline_request.response->cmdline;
    size_t n = 0;
    while (word[n]) {
        n++;
    }
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        size_t i = 0;
        while (i < n && p[i] == word[i]) {
            i++;
        }
        if (i == n && (p[n] == ' ' || p[n] == '\0')) {
            return 1;
        }
        while (*p && *p != ' ') {
            p++;
        }
    }
    return 0;
}

__attribute__((
    used,
    section(".limine_requests_start"))) static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((
    used, section(".limine_requests_end"))) static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;
