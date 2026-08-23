#include "board.h"
#include "clocks.h"

/*
 * =============================================================================
 *  GPIO + clock-system bring-up for the MSP430FR5969
 * =============================================================================
 *  Target clock tree:
 *    MCLK  (CPU)        = DCO 16 MHz          (needs 1 FRAM wait state)
 *    SMCLK (peripherals)= DCO / 2 = 8 MHz     (SPI bit clock)
 *    ACLK  (timer)      = LFXT bypass 32768 Hz (external square wave on PJ.4)
 * =============================================================================
 */

static void gpio_init(void)
{
    /* Drive every pin as an output at logic 0 first. Unconfigured CMOS inputs
     * float and burn current / pick up noise; TI's low-power app notes
     * recommend "all unused pins output low" as the safe baseline. The real
     * pin functions are then layered on top of this default.
     *
     * PxDIR: 1 = output, 0 = input.  PxOUT: output level (or pull direction
     * when the resistor enable PxREN is set). */
    P1DIR = 0xFF; P1OUT = 0x00;
    P2DIR = 0xFF; P2OUT = 0x00;
    P3DIR = 0xFF; P3OUT = 0x00;
    P4DIR = 0xFF; P4OUT = 0x00;
    PJDIR = 0xFF; PJOUT = 0x00;

    /* Route pins to their eUSCI peripheral functions. Each pin has a 2-bit
     * function selector split across two registers (PxSEL1:PxSEL0); per the
     * FR5969 datasheet port tables the eUSCI functions live at SEL1=1,SEL0=0:
     *
     *   SEL1 SEL0   function on P1.6/P1.7/P2.2
     *    0    0     plain GPIO
     *    1    0     UCB0SIMO / UCB0SOMI / UCB0CLK
     *
     * When a peripheral function is selected, the peripheral controls the
     * pin direction automatically (PxDIR is ignored for that pin).
     *
     * Only eUSCI_B0 is muxed out. P2.0/P2.1 reach the eZ-FET backchannel
     * UART on the LaunchPad, but nothing drives them any more, so they stay
     * plain outputs at 0 like every other unused pin. */
    P1SEL1 |= BIT6 | BIT7;                      /* P1.6 = UCB0SIMO (-> SDI)
                                                 * P1.7 = UCB0SOMI (<- SDOA)  */
    P1SEL0 &= (uint8_t)~(BIT6 | BIT7);          /* make sure SEL0 bits are 0  */
    P2SEL1 |= BIT2;                             /* P2.2 = UCB0CLK (-> CLOCK)  */
    P2SEL0 &= (uint8_t)~BIT2;

    /* ADC chip select: keep ~CS deasserted (high) until adc168_init() is
     * ready to talk. All other strobe outputs (CONVST P2.6, RD P4.2) idle at
     * the 0 written above, which is their inactive level. */
    P1OUT |= BIT4;

    /* BUSY input (P1.5): flip direction back to input and enable the internal
     * resistor as a pulldown so the pin reads a defined 0 when the ADC is not
     * connected (a floating BUSY could fake "conversion running" forever).
     *   P1DIR.5 = 0 -> input
     *   P1REN.5 = 1 -> internal resistor enabled
     *   P1OUT.5 = 0 -> resistor pulls DOWN (with REN set, OUT picks up/down) */
    P1DIR &= (uint8_t)~BIT5;
    P1REN |= BIT5;
    P1OUT &= (uint8_t)~BIT5;

    /* Push buttons S1 (P4.5) and S2 (P1.1): inputs with the internal PULL-UP
     * enabled. The LaunchPad wires each switch straight to GND with no
     * external pull-up, so without this the pins would float and read
     * randomly. With PxREN set, PxOUT chooses the resistor direction - 1 is
     * up - so the pin idles HIGH and a press pulls it LOW.
     *   PxDIR.n = 0 -> input
     *   PxREN.n = 1 -> internal resistor enabled
     *   PxOUT.n = 1 -> resistor pulls UP
     * Polled every tick in main(); no port interrupt is needed because the
     * firmware is already awake at 100 Hz. */
    P4DIR &= (uint8_t)~BIT5;
    P4REN |= BIT5;
    P4OUT |= BIT5;
    P1DIR &= (uint8_t)~BIT1;
    P1REN |= BIT1;
    P1OUT |= BIT1;

    /* LFXIN (PJ.4): select the crystal-oscillator function so the external
     * 32.768 kHz square wave reaches the clock system. Function select for
     * the LFX pins is PJSEL1=0, PJSEL0=1 (bypass vs crystal mode is chosen
     * later via the LFXTBYPASS bit in CSCTL4, not here). */
    PJSEL0 |= BIT4;
    PJSEL1 &= (uint8_t)~BIT4;
}

uint8_t clock_init(void)
{
    uint8_t status = 0;

    gpio_init();

    /* FRAM-family gotcha: out of reset, all I/O is held in high-impedance by
     * a latch, and everything configured above only takes effect once the
     * LOCKLPM5 bit in the power-management register is cleared. Do this
     * after configuring the ports so the pins snap directly to their final
     * states with no glitch. */
    PM5CTL0 &= ~LOCKLPM5;

    /* FRAM wait state: FRAM reads are only spec'd to 8 MHz. To run MCLK at
     * 16 MHz the FRAM controller must insert 1 wait state (NWAITS_1).
     * FRCTL0 is password-protected: every write must carry FRCTLPW (0xA5xx)
     * in the upper byte or the write is ignored. Set this BEFORE raising the
     * CPU clock, never after. */
    FRCTL0 = FRCTLPW | NWAITS_1;

    /* The clock-system (CS) registers are also password-protected: writing
     * CSKEY_H (0xA5) to the high byte of CSCTL0 unlocks them; writing any
     * wrong key forces a reset (PUC). */
    CSCTL0_H = CSKEY_H;

    /* DCO (internal digitally-controlled oscillator) frequency select:
     * DCORSEL picks the high-frequency range, DCOFSEL_4 picks 16 MHz within
     * that range (per the FR5969 datasheet DCO table). */
    CSCTL1 = DCOFSEL_4 | DCORSEL;

    /* Clock-source multiplexers, one field per system clock:
     *   SELA__LFXTCLK -> ACLK  from the LFXT pin (our external 32 kHz)
     *   SELS__DCOCLK  -> SMCLK from the DCO
     *   SELM__DCOCLK  -> MCLK  from the DCO                                 */
    CSCTL2 = SELA__LFXTCLK | SELS__DCOCLK | SELM__DCOCLK;

    /* Per-clock dividers:
     *   ACLK  /1 -> 32768 Hz
     *   SMCLK /2 -> 8 MHz   (this is the SPI reference)
     *   MCLK  /1 -> 16 MHz                                                   */
    CSCTL3 = DIVA__1 | DIVS__2 | DIVM__1;

    /* Oscillator control:
     *   LFXTBYPASS = 1 : LFXIN accepts an external digital clock instead of
     *                    driving a crystal (our case: 32.768 kHz square wave).
     *   LFXTOFF    = 0 : (implicit, bit not set) LFXT input path enabled.
     *   HFXTOFF    = 1 : keep the unused high-frequency crystal input OFF —
     *                    if HFXT were enabled with nothing attached its fault
     *                    flag would latch the global OFIFG forever.          */
    CSCTL4 = LFXTBYPASS | HFXTOFF;

    /* Oscillator-fault handshake: LFXTOFFG (in CSCTL5) latches whenever the
     * LFXT input is absent/unstable, and it feeds the global oscillator-fault
     * flag OFIFG (in SFRIFG1). While a fault is latched, ACLK is internally
     * substituted from a fallback source. The standard bring-up idiom is:
     * clear both flags, and if the source really is stable they STAY cleared;
     * if not, they immediately re-latch and we try again.
     *
     * The loop is bounded so a missing oscillator cannot hang the boot. */
    {
        uint16_t tries = 50000u;
        do {
            CSCTL5 &= ~(LFXTOFFG | HFXTOFFG);   /* clear the sticky faults    */
            SFRIFG1 &= ~OFIFG;                  /* clear the summary flag     */
        } while ((SFRIFG1 & OFIFG) && --tries); /* re-latched? -> not stable  */

        if (tries == 0u) {
            /* External 32 kHz never settled. Re-route ACLK to the internal
             * VLO (~9.4 kHz, very inaccurate — we don't time from it; the
             * tick timer will use SMCLK instead, see timer_init()), clear
             * the now-expected faults once more, and report the condition. */
            CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
            CSCTL5 &= ~(LFXTOFFG | HFXTOFFG);
            SFRIFG1 &= ~OFIFG;
            status |= ST_NO_LFXT;
        }
    }

    /* Re-lock the clock registers (any non-0xA5 high byte locks). */
    CSCTL0_H = 0;

    return status;
}
