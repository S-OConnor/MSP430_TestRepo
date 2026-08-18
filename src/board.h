#ifndef BOARD_H
#define BOARD_H

#include <msp430.h>     /* device header: pulls in msp430fr5969.h register/bit
                         * definitions (P1OUT, WDTCTL, BIT4, ...) based on the
                         * -mmcu= flag passed to the compiler                  */
#include <stdint.h>

/*
 * =============================================================================
 *  Board definition: MSP-EXP430FR5969 LaunchPad  <->  ADC168M102REVM-PDK (J5)
 * =============================================================================
 *
 *  Signal map (see README.md for the full wiring table):
 *
 *    MSP430 pin  eUSCI function  direction  EVM J5 pin  ADC signal
 *    ----------  --------------  ---------  ----------  -----------------------
 *    P2.2        UCB0CLK         out    ->  7           CLOCK  (burst-gated)
 *    P1.6        UCB0SIMO        out    ->  15          SDI
 *    P1.7        UCB0SOMI        in     <-  1           SDOA
 *    P1.4        (GPIO)          out    ->  9           ~CS    (low = active)
 *    P2.6        (GPIO)          out    ->  13          CONVST (rising edge
 *                                                       samples the inputs)
 *    P4.2        (GPIO)          out    ->  11          RD     (falling edge
 *                                                       starts data output)
 *    P1.5        (GPIO)          in     <-  5           BUSY   (high while a
 *                                                       conversion runs)
 *    strap                                  17 -> GND   M0 = 0 (with M1 pulled
 *                                                       high on the EVM this
 *                                                       selects "Mode II":
 *                                                       manual channel select,
 *                                                       data on SDOA only)
 *
 *  MCU clock tree (set up in clocks.c):
 *    MCLK  = 16 MHz  (DCO)         - CPU clock
 *    SMCLK =  8 MHz  (DCO / 2)     - feeds eUSCI SPI + UART bit clocks
 *    ACLK  = 32.768 kHz            - external square wave on LFXIN (bypass),
 *                                    feeds the 100 Hz sample-tick timer
 * =============================================================================
 */

/* ---- tunables ---------------------------------------------------------- */

/* eUSCI_B0 bit-clock divider: SPI SCLK = SMCLK / ADC_SCLK_DIV.
 * 1 -> 8 MHz (default). The ADC accepts 0.5..20 MHz in half-clock mode.
 * If bring-up shows corrupted frames (risk A in docs/PLAN.md: the CONVST/RD
 * strobe-width spec is written against a free-running clock), slow down here:
 * 2 -> 4 MHz, 4 -> 2 MHz. Nothing else needs to change. */
#define ADC_SCLK_DIV        1u

/* Sample-tick period in ACLK cycles. 32768 Hz / 328 = 99.902 Hz, the closest
 * exact fit to 100 Hz (32768 = 2^15, so no integer divider hits 100.000 Hz).
 * Using 320 instead would give a "round" 102.4 Hz. */
#define TICK_PERIOD_ACLK    328u

/* Fallback tick period when the external 32 kHz clock is missing: the timer
 * then runs from SMCLK/8 = 1 MHz, and 1 MHz / 10000 = exactly 100 Hz (but
 * only as accurate as the internal DCO, roughly +-2 %). */
#define TICK_PERIOD_SMCLK   10000u

/* How many polling-loop iterations to wait for BUSY to drop after a
 * conversion. The conversion itself needs ~18 of our 24 burst clocks
 * (3 us @ 8 MHz); each loop iteration is a few CPU cycles at 16 MHz, so 400
 * iterations is a generous ~50 us upper bound before declaring a timeout. */
#define ADC_BUSY_TIMEOUT    400u

/* CPU frequency, used by __delay_cycles() busy-waits (compile-time constant). */
#define MCLK_HZ             16000000uL

/* ---- status flags (reported in the boot banner / error LED) ------------ */

#define ST_NO_LFXT          0x01u   /* external 32 kHz never settled; ACLK is
                                     * on VLO and the tick runs from SMCLK   */
#define ST_ADC_NOLINK       0x02u   /* CONFIG readback mismatch during init:
                                     * wiring / M0 strap / EVM power suspect */

/* ---- pin helper macros -------------------------------------------------
 * Each is a single read-modify-write of a port register; the compiler turns
 * these into one BIS.B/BIC.B/XOR.B instruction on the port address.        */

/* ~CS (P1.4): the ADC ignores SDI/RD and tri-states SDOA while ~CS is high.
 * We drop it once at init and keep it low for the whole session. */
#define ADC_CS_LOW()        (P1OUT &= (uint8_t)~BIT4)   /* clear bit 4        */
#define ADC_CS_HIGH()       (P1OUT |= BIT4)             /* set bit 4          */

/* CONVST (P2.6) rising edge moves the ADC's sample/holds from track to hold
 * and arms the conversion (it actually starts on the next CLOCK rising edge).
 * RD (P4.2) falling edge makes the ADC drive the first data bit on SDOA.
 *
 * Both strobes are two back-to-back port writes: BIS.B then BIC.B, roughly
 * 190 ns at 16 MHz MCLK. The datasheet's "max one CLOCK period" pulse-width
 * rule assumes a free-running CLOCK; we only strobe while our gated SCLK is
 * sitting idle low, so no clock edge can ever land inside the pulse. */
#define ADC_CONVST_PULSE()  do { P2OUT |= BIT6; P2OUT &= (uint8_t)~BIT6; } while (0)
#define ADC_RD_PULSE()      do { P4OUT |= BIT2; P4OUT &= (uint8_t)~BIT2; } while (0)

/* BUSY (P1.5): reads nonzero while a conversion is in progress. */
#define ADC_BUSY()          (P1IN & BIT5)

/* LaunchPad LEDs: LED1 (red, P1.0) = heartbeat, LED2 (green, P4.6) = error. */
#define LED_HEART_TOGGLE()  (P1OUT ^= BIT0)
#define LED_ERR_ON()        (P4OUT |= BIT6)
#define LED_ERR_OFF()       (P4OUT &= (uint8_t)~BIT6)

#endif /* BOARD_H */
