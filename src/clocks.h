#ifndef CLOCKS_H
#define CLOCKS_H

#include <stdint.h>

/* Configure GPIO map, unlock LPM5, set FRAM wait state, and bring up
 * MCLK 16 MHz / SMCLK 8 MHz / ACLK = LFXT bypass (external 32.768 kHz).
 * Returns 0 on success or ST_NO_LFXT if the external clock never settled
 * (ACLK then runs from VLO and the tick timer must use SMCLK instead). */
uint8_t clock_init(void);

#endif /* CLOCKS_H */
