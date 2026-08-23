#include "board.h"
#include "clocks.h"

/*
 * =============================================================================
 *  GPIO + clock-system bring-up for the MSP430FR5969
 * =============================================================================
 *  Target clock tree:
 *    MCLK  (CPU)        = DCO 16 MHz          (needs 1 FRAM wait state)
 *    SMCLK (peripherals)= DCO / 2 = 8 MHz     (SPI bit clock)
 *    ACLK  (timer)      = LFXT crystal 32768 Hz (LaunchPad crystal Y4 on
 *                         PJ.4/PJ.5)
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

    /* LFXIN (PJ.4) and LFXOUT (PJ.5): hand both pins to the low-frequency
     * crystal oscillator. On the LaunchPad those two pins carry the board's
     * own 32.768 kHz crystal Y4 (LP guide §2.2.2, p. 8; schematic p. 37),
     * which is the sample timebase - nothing is wired to them.
     *
     * Crystal mode uses BOTH pins (the oscillator drives LFXOUT and senses
     * LFXIN), so both select bits are set here; function select for the LFX
     * pins is PJSEL1=0, PJSEL0=1. */
    PJSEL0 |= BIT4 | BIT5;
    PJSEL1 &= (uint8_t)~(BIT4 | BIT5);
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
     *   SELA__LFXTCLK -> ACLK  from LFXT (the onboard 32.768 kHz crystal)
     *   SELS__DCOCLK  -> SMCLK from the DCO
     *   SELM__DCOCLK  -> MCLK  from the DCO                                 */
    CSCTL2 = SELA__LFXTCLK | SELS__DCOCLK | SELM__DCOCLK;

    /* Per-clock dividers:
     *   ACLK  /1 -> 32768 Hz
     *   SMCLK /2 -> 8 MHz   (this is the SPI reference)
     *   MCLK  /1 -> 16 MHz                                                   */
    CSCTL3 = DIVA__1 | DIVS__2 | DIVM__1;

    /* Oscillator control:
     *   LFXTDRIVE_2    : oscillator drive level. The data sheet brackets the
     *                    four settings by effective load capacitance (MCU
     *                    Table 5-4, p. 26): {2} covers 6..9 pF, and the
     *                    LaunchPad's Y4 is a 7 pF crystal (LP schematic,
     *                    p. 37), so {2} is the matched setting.
     *   LFXTBYPASS = 0 : (implicit, bit not set) crystal mode — the on-chip
     *                    oscillator drives Y4.
     *   LFXTOFF    = 0 : (implicit, bit not set) LFXT enabled.
     *   HFXTOFF    = 1 : keep the unused high-frequency crystal input OFF —
     *                    if HFXT were enabled with nothing attached its fault
     *                    flag would latch the global OFIFG forever.          */
    CSCTL4 = LFXTDRIVE_2 | HFXTOFF;

    /* Oscillator-fault handshake: LFXTOFFG (in CSCTL5) latches whenever LFXT
     * is not oscillating cleanly, and it feeds the global oscillator-fault
     * flag OFIFG (in SFRIFG1). While a fault is latched, ACLK is internally
     * substituted from a fallback source. The bring-up idiom is: clear both
     * flags, wait, re-test — if the crystal is running they STAY cleared; if
     * not, they re-latch and we go round again.
     *
     * A 32 kHz crystal is SLOW to start: it needs hundreds of milliseconds to
     * reach amplitude, and until it does LFXTOFFG re-latches as fast as this
     * loop can clear it. Hence a real 10 ms delay per pass — a tight spin
     * would give up on every healthy cold boot. Worst case the whole window
     * is LFXT_SETTLE_TRIES x 10 ms (~1 s), and only then does the firmware
     * declare the crystal dead: bounded, so it can never hang the boot. */
    {
        uint16_t tries = LFXT_SETTLE_TRIES;
        do {
            CSCTL5 &= ~(LFXTOFFG | HFXTOFFG);   /* clear the sticky faults    */
            SFRIFG1 &= ~OFIFG;                  /* clear the summary flag     */
            __delay_cycles(MCLK_HZ / 100uL);    /* 10 ms at 16 MHz MCLK       */
        } while ((SFRIFG1 & OFIFG) && --tries); /* re-latched? -> not up yet  */

        if (tries == 0u) {
            /* The crystal never started — Y4 missing, damaged, or its pads
             * lifted by rework. Re-route ACLK to the internal VLO (~9.4 kHz,
             * very inaccurate — we don't time from it; the tick timer will
             * use SMCLK instead, see timer_init()), clear the now-expected
             * faults once more, and report the condition. */
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
