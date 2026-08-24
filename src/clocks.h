#ifndef CLOCKS_H
#define CLOCKS_H

#include <stdint.h>

/* Configure the GPIO map, release the I/O latch (PM5CTL0), set the FRAM wait
 * state, and bring up MCLK = SMCLK = 16 MHz from the internal DCO, with ACLK
 * parked on the VLO. Those 16 MHz and the 0.5 MHz ADC bit clock derived from
 * them in spi_init() are the only two rates in the design; the CPU runs
 * continuously and never enters a sleep state.
 *
 * No crystal is used - LFXT and HFXT are both held off - so there is nothing
 * that can fail to start, no start-up window to wait through, and no status to
 * report. Everything therefore runs at DCO accuracy (~+-2 %). */
void clock_init(void);

#endif /* CLOCKS_H */
