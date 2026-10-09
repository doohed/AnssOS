#include "pit.h"
#include "../arch/x86_64/io.h"

#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND 0x43
#define PIT_GATE_PORT 0x61 /* Bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output. */

void pit_start_periodic(uint32_t hz) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / hz);

    outb(PIT_COMMAND, 0x36); /* Channel 0, lobyte/hibyte access, mode 3 (square wave), binary. */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

int pit_poll_wait_ms(uint32_t ms) {
    uint32_t count = PIT_BASE_FREQ / 1000 * ms;
    if (count > 0xFFFF) {
        count = 0xFFFF;
    }

    /* Gate off, speaker off, then mode 0 (output goes high when the
     * count reaches zero), then raise the gate to start counting. */
    uint8_t gate = inb(PIT_GATE_PORT) & (uint8_t)~0x03;
    outb(PIT_GATE_PORT, gate);
    outb(PIT_COMMAND, 0xB0); /* Channel 2, lobyte/hibyte access, mode 0, binary. */
    outb(PIT_CHANNEL2, (uint8_t)(count & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(count >> 8));
    outb(PIT_GATE_PORT, gate | 0x01);

    /* Each port read is roughly a microsecond, so this gives up after a
     * few seconds rather than hanging on a dead timer. */
    for (uint32_t i = 0; i < 5000000; i++) {
        if (inb(PIT_GATE_PORT) & 0x20) {
            outb(PIT_GATE_PORT, gate);
            return 0;
        }
    }
    outb(PIT_GATE_PORT, gate);
    return -1;
}
