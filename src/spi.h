#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/* eUSCI_B0 3-wire master for the ADC168M102R-SEP, half-clock mode:
 * CPOL=0/CPHA=1 (UCCKPL=0, UCCKPH=0 — TI's UCCKPH is the inverse of Motorola
 * CPHA): SIMO shifts on rising edges (ADC latches SDI on falling), SOMI is
 * captured on falling edges (ADC updates SDOA on rising). MSB first.
 * SCLK = SMCLK / ADC_SCLK_DIV, idles low between transfers — exactly the
 * static-low burst clock the ADC datasheet permits (SBASAW9 §6.3.1.4). */
void spi_init(void);

/* Blocking full-duplex byte exchange (~1 us @ 8 MHz). */
uint8_t spi_xfer(uint8_t tx);

#endif /* SPI_H */
