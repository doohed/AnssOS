#ifndef DRIVERS_ACPI_H
#define DRIVERS_ACPI_H

#include <stdint.h>

/* The ACPI PM timer: a free-running counter at a fixed 3.579545 MHz that
 * every x86 PC with ACPI has, readable with a plain `in` -- no interrupt
 * routing, nothing to program. What drivers/timer.c measures the Local
 * APIC timer against. */
#define ACPI_PM_TIMER_HZ 3579545u

/* Finds the RSDP Limine handed over, walks the XSDT (or RSDT on ACPI
 * 1.0) and remembers what's needed from the tables -- so far just the
 * FADT's PM timer. Must run after pmm_init() (tables are mapped through
 * vmm_map_mmio()). Returns 0 if the FADT was found. */
int acpi_init(void);

/* Whether the FADT describes a usable PM timer. */
int acpi_pm_timer_present(void);

/* PM timer ticks elapsed between two acpi_pm_timer_read() values,
 * handling the counter wrapping (it's 24 bits on some machines). */
uint32_t acpi_pm_timer_read(void);
uint32_t acpi_pm_timer_delta(uint32_t from, uint32_t to);

/* For the boot log: "port 0x608, 24-bit" or "none". */
uint16_t acpi_pm_timer_port(void);
int acpi_pm_timer_bits(void);

#endif
