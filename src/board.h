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
 *    P1.4        (GPIO)          out    ->  9           ~CS    (low = active;
 *                                                       one access per
 *                                                       assertion)
 *    P2.6        (GPIO)          out    ->  13          CONVST (rising edge
 *                                                       samples the inputs;
 *                                                       held over CLOCK 1)
 *    P4.2        (GPIO)          out    ->  11          RD     (rising edge
 *                                                       starts data output;
 *                                                       held over CLOCK 1)
 *    P1.5        (GPIO)          in     <-  5           BUSY   (high while a
 *                                                       conversion runs)
 *    P2.0        UCA0TXD         out    ->  (on-board)  backchannel UART TX
 *    P2.1        UCA0RXD         in     <-  (on-board)  backchannel UART RX
 *                                                       (unused; muxed only
 *                                                       so the pin stops
 *                                                       driving the eZ-FET)
 *    strap                                  17 -> GND   M0 = 0 (with M1 pulled
 *                                                       high on the EVM this
 *                                                       selects "Mode II":
 *                                                       manual channel select,
 *                                                       data on SDOA only)
 *
 *  MCU clock tree (set up in clocks.c). Everything divides down from one
 *  16 MHz system clock - the ADC bus runs at 0.5 MHz, the serial link at
 *  115200 baud:
 *    MCLK  = 16 MHz  (DCO)         - CPU clock, running continuously
 *    SMCLK = 16 MHz  (DCO, /1)     - feeds the eUSCI SPI bit clock (/32 =
 *                                    0.5 MHz), the 100 Hz sample-tick timer
 *                                    (via /8 = 2 MHz), AND the UART baud
 *                                    generator (115200, oversampling mode)
 *    ACLK  = ~9.4 kHz (VLO)        - parked on the internal very-low-frequency
 *                                    oscillator; nothing is timed from it.
 *                                    No crystal is used: LFXT and HFXT are
 *                                    both held off, so PJ.4/PJ.5 stay plain
 *                                    GPIO and the LaunchPad's Y4 is idle.
 *
 *  Analog inputs (EVM headers, odd pins signal / even pins GND):
 *
 *    ADC channel  EVM header pin  buffered by
 *    -----------  --------------  ------------------------------------------
 *    CHA1         J2 pin 5        OPA4H014-SEP U2C  (needs OPA_V+/- on J3/J4)
 *    CHB1         J1 pin 5        OPA4H014-SEP U1C
 *
 *  This firmware acquires exactly those two channels - see ADC_PAIR below.
 * =============================================================================
 */

/* ---- tunables ---------------------------------------------------------- */

/* eUSCI_B0 bit-clock divider: SPI SCLK = SMCLK / ADC_SCLK_DIV.
 * 32 -> 16 MHz / 32 = 0.5 MHz, the bottom of the ADC's 0.5..20 MHz half-clock
 * window. The slow clock buys
 * setup/hold margin on the jumper wires to the EVM, and it is what makes the
 * CPU-timed strobe release below possible, at the cost of a longer bus burst
 * per tick (~150 us, still ~1.5 % of the 10 ms tick).
 *
 * NOT a free one-line change any more: spi_burst_strobe() releases CONVST/RD
 * by watching SCLK edges from the CPU, which needs a half period long
 * compared with its ~8-cycle poll loop. 32 (1 us per half period) is the
 * bottom of that; spi.c carries an #error below it. To go faster, drive the
 * strobe release from a timer capture/compare unit instead. */
#define ADC_SCLK_DIV        32u

/* Which channel pair the ADC converts, 0..3.
 *
 * The part holds two converters (A and B) behind a 4:1 mux each, and one
 * conversion digitizes pair k = CHAk + CHBk *simultaneously*. This build
 * acquires a single pair per tick, so the mux selection is a constant:
 * pair 1 = CHA1 (EVM J2.5) + CHB1 (EVM J1.5).
 *
 * Changing this constant is the only edit needed to move to another pair -
 * it feeds the C field of the init CONFIG word, the C field of every
 * per-conversion command, and which physical inputs you must wire up. */
#define ADC_PAIR            1

#if (ADC_PAIR < 0) || (ADC_PAIR > 3)
#error "ADC_PAIR must be 0..3 (pair k = CHAk + CHBk)"
#endif

/* Sample-tick period in timer clocks. Timer_A0 runs from SMCLK/8 = 2 MHz, and
 * 2 MHz / 20000 = exactly 100.000 Hz - as accurate as the internal DCO, which
 * is roughly +-2 %. The tick only has to be regular enough to space the ADC
 * bursts evenly; nothing downstream measures absolute time from it. */
#define TICK_PERIOD_SMCLK   20000u

/* Idle-phase period, in sample ticks: how often the firmware rewrites and
 * reads back the ADC CONFIG register while it waits for a button press.
 * 100 ticks x ~10 ms = ~1 s. The tick timer keeps running at ~100 Hz in both
 * phases - only what a tick DOES changes - so this is the one constant that
 * sets the idle repeat rate, and the 10 ms tick doubles as the button poll
 * and its debounce interval. */
#define IDLE_CONFIG_TICKS   100u

/* Consecutive 10 ms polls a button must read "down" before the firmware
 * accepts the press. 2 -> the contact must be stable for >=10 ms, which is
 * past the bounce of these tact switches, and no press a human can make is
 * short enough to miss. */
#define BTN_DEBOUNCE_POLLS  2u

/* How many polling-loop iterations to wait for BUSY to drop after a
 * conversion. The conversion itself needs ~18 of our 24 burst clocks
 * (36 us @ 0.5 MHz SCLK), so it is already over by the time the 24-clock
 * burst (48 us) ends and this loop starts - the loop only exists so a dead
 * ADC cannot hang the tick. Each iteration is a few CPU cycles at 16 MHz, so
 * 400 iterations is a ~50 us upper bound before declaring a timeout. */
#define ADC_BUSY_TIMEOUT    400u

/* CPU frequency, used by __delay_cycles() busy-waits (compile-time constant). */
#define MCLK_HZ             16000000uL

/* ---- status flags (latched into g_status; any nonzero -> error LED) ---- */

/* 0x01 is retired: it was ST_NO_LFXT, raised when the 32.768 kHz crystal
 * failed to start. No crystal is used any more, so the condition - and the
 * ~1 s boot delay spent waiting for it - no longer exists. */
#define ST_ADC_NOLINK       0x02u   /* CONFIG readback mismatch during init:
                                     * wiring / M0 strap / EVM power suspect */

/* ---- pin helper macros -------------------------------------------------
 * Each is a single read-modify-write of a port register; the compiler turns
 * these into one BIS.B/BIC.B/XOR.B instruction on the port address.        */

/* ~CS (P1.4): the ADC ignores SDI/RD and tri-states SDOA while ~CS is high.
 * It is asserted PER ACCESS — low just before the strobe and the burst it
 * opens, high again as soon as that burst's last CLOCK edge has gone by — so
 * the bus is fully idle (clock low, ~CS high, strobes low) between accesses.
 * That is the shape the PHI reference board produces in docs/workingADC.jpg,
 * and adc168m102.c's access() is the only caller. The single timing the
 * datasheet attaches to the line is tD6 = 6 ns from the ~CS rising edge to
 * SDOA tri-stating (SBASAW9 §5.7). */
#define ADC_CS_LOW()        (P1OUT &= (uint8_t)~BIT4)   /* clear bit 4        */
#define ADC_CS_HIGH()       (P1OUT |= BIT4)             /* set bit 4          */

/* CONVST (P2.6) and RD (P4.2) are the two access-opening strobes, and the
 * datasheet times BOTH of them against the CLOCK edges of the access they
 * open (SBASAW9 Figure 5-1, "Detailed Timing Diagram: Half-Clock Mode"):
 *
 *      CLOCK    ___|‾|_|‾|_|‾|_|‾|_        edge 1     edge 2
 *      CONVST   __|‾‾‾‾‾‾‾‾|______         ^t1 before  ^released here
 *      RD       __|‾‾‾‾‾‾‾‾|______
 *
 *   - the strobe goes HIGH at least t1 BEFORE the first rising CLOCK edge of
 *     the burst, so it is already high when the ADC samples it there;
 *   - it is released within one CLOCK period after that — the diagram's
 *     hatched trailing edge — i.e. on the SECOND rising edge of the same
 *     access. CONVST's rising edge freezes the sample/holds and the
 *     conversion starts on the first CLOCK rising edge; RD's rising edge
 *     makes the ADC start driving SDOA (t3), and the same access opens the
 *     16-CLOCK SDI command window.
 *
 * So a strobe is NOT a pulse that fits in the gap between bursts: it spans
 * the first clock of its burst. BOTH its edges are therefore written from
 * inside spi_burst_strobe() (spi.c), which is the only code that can place
 * them against the clock it starts — hence the port+bit pairs below, which
 * is how a strobe is handed to it. The plain set/clear macros are there for
 * anything that needs to move a strobe outside a burst.
 *
 * Budget: t2/t3 cap the high time at one CLOCK period (2 us at 0.5 MHz) and
 * t1 asks for at least 12 ns of it before the first clock edge. We spend a
 * few hundred nanoseconds of lead (one instruction plus the eUSCI's own
 * start-up) and one CLOCK period of hold.
 *
 * (This mirrors the PHI reference board's capture in docs/workingADC.jpg,
 * where CONVST and RD each straddle the first clocks of their burst.) */
#define ADC_CONVST_PORT     (&P2OUT)
#define ADC_CONVST_BIT      BIT6
#define ADC_RD_PORT         (&P4OUT)
#define ADC_RD_BIT          BIT2

#define ADC_CONVST_HIGH()   (P2OUT |= BIT6)
#define ADC_CONVST_LOW()    (P2OUT &= (uint8_t)~BIT6)
#define ADC_RD_HIGH()       (P4OUT |= BIT2)
#define ADC_RD_LOW()        (P4OUT &= (uint8_t)~BIT2)

/* SCLK (P2.2) as an INPUT level. The pin is muxed to UCB0CLK, but PxIN always
 * reflects the physical pin whatever function drives it, so the CPU can watch
 * the bit clock it is generating and time a strobe release against real
 * edges instead of against a counted delay. spi.c is the only user. */
#define SPI_SCLK_HIGH()     ((P2IN & BIT2) != 0u)

/* BUSY (P1.5): reads nonzero while a conversion is in progress. */
#define ADC_BUSY()          (P1IN & BIT5)

/* LaunchPad LEDs. The board's two user-interface clusters sit on opposite
 * ports (SLAU535B schematic, p. 37): the left one is button S1 (P4.5) beside
 * LED1, RED, on P4.6; the right one is button S2 (P1.1) beside LED2, GREEN,
 * on P1.0. This firmware uses the green LED2 as the heartbeat (0.5 Hz while
 * idle, 1 Hz while streaming) and the red LED1 as the latched error light. */
#define LED_HEART_TOGGLE()  (P1OUT ^= BIT0)
#define LED_ERR_ON()        (P4OUT |= BIT6)
#define LED_ERR_OFF()       (P4OUT &= (uint8_t)~BIT6)

/* LaunchPad push buttons: S1 = P4.5 (left), S2 = P1.1 (right). Both switch
 * the pin to GND when pressed and float otherwise, so clocks.c enables the
 * internal PULL-UP on each and "pressed" reads as a LOW level - hence the
 * inverted tests below. Either button starts streaming; the firmware never
 * needs to tell them apart. */
#define BTN_S1_DOWN()       ((P4IN & BIT5) == 0u)
#define BTN_S2_DOWN()       ((P1IN & BIT1) == 0u)
#define BTN_ANY_DOWN()      (BTN_S1_DOWN() || BTN_S2_DOWN())

/* ---- inlining ------------------------------------------------------------
 * Force a helper to be inlined even at -Os, where gcc happily leaves a small
 * static as a real call.
 *
 * Used on the pass-through wrappers that exist only to name a step of one ADC
 * access. Left out of line, each costs its own frame plus a 2-byte return
 * address for the whole time the levels below it run:
 *   main -> adc168_read -> read_access -> adc_access -> spi_burst_strobe
 *        -> burst_pump
 * Collapsing the wrappers takes the peak chain from 76 to 56 bytes for about
 * 80 bytes of duplicated code in FRAM, which is the plentiful resource here
 * (64 KB FRAM vs 2 KB SRAM).
 *
 * Keep the scope modest, though. 2 KB of SRAM against a 76-byte high-water
 * mark was never tight — this buys headroom, it does not fix a problem — so it
 * is not worth distorting the design to push the number lower. Two limits came
 * out of measuring it:
 *
 *   - Inlining ACROSS modules means moving a definition into a header, which
 *     costs that header its independence. spi_wait_ready() stays in spi.c for
 *     that reason: 4 bytes were not worth spi.h pulling in board.h.
 *   - always_inline overrides the compiler permanently, and the cost scales
 *     with call sites, silently. write_word() has eight in adc168_init()
 *     alone; marking it grew the image by 651 bytes and saved no stack at all.
 *     Re-measure with -fstack-usage before adding this to anything new.
 *
 * The one place it is about timing rather than stack is the SCLK edge poll in
 * spi.c, where a call/ret pair would sit between the edge and the strobe
 * release. */
#define ALWAYS_INLINE       static inline __attribute__((always_inline))

#endif /* BOARD_H */
