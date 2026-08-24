#include "board.h"
#include "clocks.h"

/*
 * =============================================================================
 *  GPIO + clock-system bring-up for the MSP430FR5969
 * =============================================================================
 *  Target clock tree - the whole system runs at ONE rate, and the ADC bus at
 *  one divided rate; those two numbers are the only timings in the design:
 *    MCLK  (CPU)        = DCO 16 MHz          (needs 1 FRAM wait state)
 *    SMCLK (peripherals)= DCO 16 MHz          (undivided; SPI bit clock /32
 *                                              -> 0.5 MHz, the tick timer
 *                                              via /8, and the UART baud
 *                                              generator -> 115200)
 *    ACLK               = VLO ~9.4 kHz        (parked; nothing is timed
 *                                              from it)
 *
 *  The CPU is never stopped. It runs flat out at 16 MHz from reset onward and
 *  busy-waits between ticks; no sleep or low-power state is entered anywhere
 *  in this firmware.
 *
 *  NO CRYSTAL IS USED. Both crystal oscillators are held off (LFXTOFF,
 *  HFXTOFF), so PJ.4/PJ.5 stay plain GPIO, the LaunchPad's Y4 sits idle, and
 *  the whole clock tree comes from the on-chip DCO/VLO. Everything the
 *  firmware times - the 100 Hz sample tick, the SPI bursts, the millisecond
 *  delays - therefore runs at DCO accuracy (roughly +-2 %), which is all any
 *  of it needs: the tick only has to space the ADC bursts evenly.
 *
 *  Dropping the crystal also removes the ~1 s start-up window bring-up used
 *  to spend waiting for a watch crystal to reach amplitude, so boot is now
 *  immediate and clock_init() can no longer fail.
 * =============================================================================
 */

static void gpio_init(void)
{
    /* Drive every pin as an output at logic 0 first. Unconfigured CMOS inputs
     * float, which lets them oscillate and pick up noise; TI's application
     * notes recommend "all unused pins output low" as the safe baseline. The
     * real pin functions are then layered on top of this default.
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
     * Two modules are muxed out: eUSCI_B0 for the ADC bus, and eUSCI_A0 on
     * P2.0/P2.1, which reach the LaunchPad's eZ-FET debug chip and come out
     * of it as a USB CDC serial port on the host. RXD is muxed even though
     * this firmware only transmits — leaving the pin as a driven output
     * would fight the eZ-FET whenever the host's terminal sent a keystroke. */
    P1SEL1 |= BIT6 | BIT7;                      /* P1.6 = UCB0SIMO (-> SDI)
                                                 * P1.7 = UCB0SOMI (<- SDOA)  */
    P1SEL0 &= (uint8_t)~(BIT6 | BIT7);          /* make sure SEL0 bits are 0  */
    P2SEL1 |= BIT0 | BIT1 | BIT2;               /* P2.0 = UCA0TXD (-> host)
                                                 * P2.1 = UCA0RXD (<- host)
                                                 * P2.2 = UCB0CLK (-> CLOCK)  */
    P2SEL0 &= (uint8_t)~(BIT0 | BIT1 | BIT2);

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
     * firmware runs continuously and looks at the pins 100 times a second. */
    P4DIR &= (uint8_t)~BIT5;
    P4REN |= BIT5;
    P4OUT |= BIT5;
    P1DIR &= (uint8_t)~BIT1;
    P1REN |= BIT1;
    P1OUT |= BIT1;

    /* PJ.4/PJ.5 (LFXIN/LFXOUT) need no special handling: with LFXT off they
     * are left in the "output low" default written above, which is the
     * recommended state for an unused pin. The LaunchPad's crystal Y4 is
     * still fitted across them (LP guide §2.2.2, p. 8; schematic p. 37) but
     * it is a passive part - held statically at 0 V on both ends it simply
     * does not oscillate and draws no current. Neither pin reaches a header,
     * so nothing else can be affected. */
}

void clock_init(void)
{
    gpio_init();

    /* FRAM-family gotcha: out of reset, all I/O is held in high-impedance by
     * a latch, and everything configured above only takes effect once that
     * latch is released by clearing the LOCKLPM5 bit in the power-management
     * register. The bit is named for a state this firmware never enters; here
     * it is purely the "release the I/O latch" switch, and without this write
     * no pin configuration reaches the outside world. Do this after
     * configuring the ports so the pins snap directly to their final states
     * with no glitch. */
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
     * that range (per the FR5969 datasheet DCO table). This is the CPU clock
     * AND the peripheral clock — every clock in the system now derives from
     * it apart from the parked ACLK. */
    CSCTL1 = DCOFSEL_4 | DCORSEL;

    /* Clock-source multiplexers, one field per system clock:
     *   SELA__VLOCLK -> ACLK  from the internal VLO (~9.4 kHz)
     *   SELS__DCOCLK -> SMCLK from the DCO
     *   SELM__DCOCLK -> MCLK  from the DCO
     *
     * ACLK is deliberately pointed at the VLO rather than left at its reset
     * default of LFXTCLK: with no crystal running, an ACLK still selecting
     * LFXT would keep requesting a dead oscillator and keep OFIFG latched.
     * Nothing is timed from ACLK — the tick timer runs from SMCLK — so the
     * VLO's poor accuracy is irrelevant; this is just a safe parking spot. */
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;

    /* Per-clock dividers - none of them divide:
     *   ACLK  /1 -> ~9.4 kHz (unused)
     *   SMCLK /1 -> 16 MHz   (SPI reference and tick-timer source)
     *   MCLK  /1 -> 16 MHz   (CPU)
     * SMCLK is deliberately left equal to MCLK so there is a single system
     * frequency to reason about; the only other rate in the design is the
     * 0.5 MHz ADC bit clock, which eUSCI_B0 divides down itself (/32).      */
    CSCTL3 = DIVA__1 | DIVS__1 | DIVM__1;

    /* Oscillator control — both crystal oscillators OFF:
     *   LFXTOFF = 1 : the low-frequency crystal oscillator is disabled. The
     *                 LaunchPad's Y4 is left fitted but never driven, and
     *                 PJ.4/PJ.5 stay plain GPIO (see gpio_init()).
     *   HFXTOFF = 1 : the high-frequency crystal input stays off too —
     *                 nothing is attached to it on this board.
     * An oscillator that is off cannot fault, so there is no start-up wait
     * and no fault-retry loop here any more. */
    CSCTL4 = LFXTOFF | HFXTOFF;

    /* Clear the sticky oscillator-fault flags once. Out of reset ACLK
     * momentarily selects LFXTCLK, which is long enough to latch LFXTOFFG
     * and the global OFIFG summary flag before the writes above take effect.
     * With both oscillators now disabled and nothing sourcing from them, the
     * flags stay clear — no re-test loop is needed. */
    CSCTL5 &= ~(LFXTOFFG | HFXTOFFG);
    SFRIFG1 &= ~OFIFG;

    /* Re-lock the clock registers (any non-0xA5 high byte locks). */
    CSCTL0_H = 0;
}
