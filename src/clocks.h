#ifndef CLOCKS_H
#define CLOCKS_H

#include <stdint.h>

/* Configure GPIO map, unlock LPM5, set FRAM wait state, and bring up
 * MCLK 16 MHz / SMCLK 8 MHz / ACLK = LFXT crystal mode on the LaunchPad's
 * onboard 32.768 kHz crystal Y4 (PJ.4/PJ.5).
 * Returns 0 on success or ST_NO_LFXT if the crystal never started within the
 * ~1 s start-up window (ACLK then runs from VLO and the tick timer must use
 * SMCLK instead). Note that a failed start-up therefore costs ~1 s of boot
 * time; a healthy crystal usually settles in a fraction of that. */
uint8_t clock_init(void);

#endif /* CLOCKS_H */
