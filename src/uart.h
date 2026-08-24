#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>

/* eUSCI_A0 (eZ-FET backchannel, P2.0/P2.1), 115200-8-N-1 from 16 MHz SMCLK. */
void uart_init(void);

/* Enqueue len bytes for interrupt-driven transmission. All-or-nothing: if the
 * ring lacks space the whole write is dropped (drop counter incremented) so
 * lines never interleave. Returns true if enqueued. */
bool uart_write(const char *buf, uint16_t len);

/* Convenience for zero-terminated strings (init/banner paths). */
bool uart_puts(const char *s);

/* Block until the TX ring drains AND the shift register empties. Boot banner
 * only — never call it from the tick path, where a full line is ~2 ms. */
void uart_flush(void);

uint16_t uart_tx_drops(void);

#endif /* UART_H */
