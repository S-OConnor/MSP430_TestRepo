#ifndef CLOCKS_H
#define CLOCKS_H

#include <stdint.h>

/* Configure GPIO map, unlock LPM5, set FRAM wait state, and bring up
 * MCLK 16 MHz / SMCLK 8 MHz from the internal DCO, with ACLK parked on the
 * VLO. No crystal is used - LFXT and HFXT are both held off - so there is
 * nothing that can fail to start, no start-up window to wait through, and no
 * status to report. Everything therefore runs at DCO accuracy (~+-2 %). */
void clock_init(void);

#endif /* CLOCKS_H */
