#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/* eUSCI_B0 3-wire master for the ADC168M102R-SEP, half-clock mode:
 * CPOL=0/CPHA=1 (UCCKPL=0, UCCKPH=0 — TI's UCCKPH is the inverse of Motorola
 * CPHA): SIMO shifts on rising edges (ADC latches SDI on falling), SOMI is
 * captured on falling edges (ADC updates SDOA on rising). MSB first.
 * SCLK = SMCLK / ADC_SCLK_DIV = 8 MHz / 16 = 0.5 MHz, idles low between
 * transfers — exactly the static-low burst clock the ADC datasheet permits
 * (SBASAW9 §6.3.1.4), at the bottom of its half-clock range. */
void spi_init(void);

/* Block until the SPI is fully idle (shift register drained, SCLK parked at
 * idle low) AND the transmit buffer will accept a byte without blocking.
 *
 * Call this immediately BEFORE a strobe that has to sit right in front of a
 * burst — RD opening a read access, CONVST arming a conversion. It moves the
 * waiting to before the strobe, so what remains between the strobe and the
 * first SCLK edge is a fixed handful of instructions instead of an
 * open-ended poll. Without it the strobe can precede the clock by a long and
 * variable gap, or even land mid-burst. */
void spi_wait_ready(void);

/* Exchange n bytes as ONE contiguous train of clocks — no stall at the byte
 * boundaries, which a byte-at-a-time loop cannot avoid (see spi.c for why).
 * Full duplex: tx[i] goes out while rx[i] comes in. Pass rx = NULL to
 * discard the received bytes. On return the shift register is empty and SCLK
 * is back at idle low, so a following strobe is safe.
 *
 * Blocking: n * 16 us at the 0.5 MHz bit clock. A 3-byte access is ~48 us,
 * and a streaming tick spends three of them — ~150 us out of 10 ms. */
void spi_burst(const uint8_t *tx, uint8_t *rx, uint8_t n);

#endif /* SPI_H */
