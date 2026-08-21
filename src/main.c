#include <stdbool.h>
#include "board.h"
#include "clocks.h"
#include "spi.h"
#include "adc168m102.h"

/*
 * =============================================================================
 *  Application: sample 2 ADC channels every ~10 ms — bus-observable, no UART
 * =============================================================================
 *  The two channels are CHA<ADC_PAIR> and CHB<ADC_PAIR> (board.h; pair 1 by
 *  default = CHA1 on EVM J2.5 and CHB1 on EVM J1.5). They form one "pair", so
 *  the ADC's two converters digitize them at the SAME instant in a single
 *  conversion, and one readout returns both.
 *
 *  There is no serial output. The results are read off the ADC bus itself with
 *  a scope or logic analyzer: SDOA carries the two 20-bit frames during the
 *  40-clock readout burst. Trigger on the CONVST rising edge — it fires once
 *  per tick with ~10 ms of quiet either side, so a single-shot capture lands
 *  on a whole acquisition every time. EXAMPLE_OUTPUTS.md shows the expected
 *  trace and walks a readout burst through to two numbers.
 *
 *  Health is reported on the two LaunchPad LEDs, and every value the firmware
 *  computes stays in a global that a debugger can read (mspdebug: `md &g_...`)
 *  — see the "observable state" block below.
 *
 *  Control flow:
 *    1. Hardware init (watchdog off, clocks, SPI, ADC configuration).
 *    2. Start the tick timer.
 *    3. Loop forever: sleep in LPM0 -> timer interrupt wakes us -> one
 *       conversion + readout -> update LEDs -> back to sleep.
 *
 *  Timing budget per 10 ms tick: ~20 us of ADC bus traffic, a few us of
 *  bookkeeping. The CPU is awake well under 1 % of the time.
 * =============================================================================
 */

/* ---- observable state ---------------------------------------------------
 * With the UART gone these globals ARE the readout path for anything the
 * scope cannot show: halt the target with mspdebug and dump them. They are
 * `volatile` so the compiler cannot fold away stores nobody reads. */

static volatile int16_t g_sample_a;     /* latest CHA<ADC_PAIR> code          */
static volatile int16_t g_sample_b;     /* latest CHB<ADC_PAIR> code          */
static volatile uint32_t g_tick;        /* completed sample count             */
static volatile uint16_t g_err_frame;   /* frames that failed the bit check   */
static volatile uint16_t g_err_busy;    /* conversions where BUSY never fell  */
static volatile uint8_t g_status;       /* ST_* flags from init               */
static volatile uint16_t g_cfg;         /* raw CONFIG readback (link check)   */

/* Set by the timer ISR, consumed by main(). `volatile` because it is written
 * in interrupt context and read in a loop the compiler would otherwise hoist. */
static volatile bool g_tick_pending;

/* ---- sample tick timer -------------------------------------------------- */

static void timer_init(uint8_t status)
{
    /* Timer_A0 in "up mode": the 16-bit counter climbs from 0 to TA0CCR0,
     * fires the CCR0 interrupt, resets to 0, and repeats. So the period is
     * (CCR0 + 1) timer clocks — hence the "- 1" on the constants. */
    if (status & ST_NO_LFXT) {
        /* Fallback (no external 32 kHz): clock the timer from SMCLK.
         *   TASSEL__SMCLK  source = SMCLK (8 MHz)
         *   ID__8          input divider /8 -> 1 MHz timer clock
         *   MC__UP         up mode
         *   TACLR          clear the counter/divider state on start
         * 1 MHz / 10000 = exactly 100 Hz (to DCO accuracy, ~+-2 %). */
        TA0CCR0 = TICK_PERIOD_SMCLK - 1u;
        TA0CCTL0 = CCIE;            /* enable the CCR0 compare interrupt */
        TA0CTL = TASSEL__SMCLK | ID__8 | MC__UP | TACLR;
    } else {
        /* Normal: clock the timer straight from ACLK = the external
         * 32.768 kHz square wave. 32768 / 328 = 99.902 Hz. */
        TA0CCR0 = TICK_PERIOD_ACLK - 1u;
        TA0CCTL0 = CCIE;
        TA0CTL = TASSEL__ACLK | MC__UP | TACLR;
    }
}

/* Timer_A0 CCR0 interrupt: fires once per sample period. It does the
 * absolute minimum — raise a flag and make sure the CPU wakes up — so all
 * the real work runs at normal priority in main() with interrupts enabled.
 *
 * __bic_SR_register_on_exit() clears the low-power-mode bits in the STATUS
 * REGISTER copy that was pushed on the stack when the interrupt was taken,
 * so when the ISR returns the CPU comes back running instead of going back
 * to sleep. */
void __attribute__((interrupt(TIMER0_A0_VECTOR))) ta0_ccr0_isr(void)
{
    g_tick_pending = true;
    __bic_SR_register_on_exit(LPM0_bits);
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    int16_t va, vb;                 /* this tick's pair, before publishing */

    /* Stop the watchdog timer. On MSP430 the WDT is RUNNING out of reset and
     * would reset the chip in ~32 ms unless serviced; we do not use it, so
     * hold it. WDTCTL is password-protected: the upper byte must be WDTPW
     * (0x5A) on every write, otherwise the chip resets immediately. WDTHOLD
     * is the "stop counting" bit. */
    WDTCTL = WDTPW | WDTHOLD;

    g_status = clock_init();        /* GPIO map, LPM5 unlock, 16/8 MHz, LFXT */
    spi_init();                     /* 8 MHz CPOL0/CPHA1 master              */
    __enable_interrupt();           /* set GIE                               */

    /* Reset + configure the ADC; also runs the link check. The raw CONFIG
     * readback is the single most useful bring-up value, so publish it:
     * expect 0x1041, and see the scope trace in EXAMPLE_OUTPUTS.md for what
     * the exchange that produced it looks like on the wire. */
    g_status |= adc168_init();
    g_cfg = adc168_link_readback();

    /* A failed link check lights the error LED immediately, before the first
     * tick — so a mis-wired board says so at power-on rather than after the
     * first bad frame. */
    if (g_status != 0u) {
        LED_ERR_ON();
    }

    timer_init(g_status);           /* start the ~100 Hz tick */

    for (;;) {
        /* Sleep until the tick ISR sets the flag. The disable/test/sleep
         * sequence avoids the classic race where the interrupt fires between
         * "test flag" and "go to sleep" and we then sleep through a tick:
         * __bis_SR_register(LPM0_bits | GIE) atomically re-enables interrupts
         * AND enters low-power mode 0 in one instruction.
         *
         * LPM0 rather than the deeper LPM3 because LPM3 stops SMCLK — which
         * is exactly what clocks the tick timer in the no-LFXT fallback, so
         * LPM3 would leave that build asleep forever. */
        __disable_interrupt();
        while (!g_tick_pending) {
            __bis_SR_register(LPM0_bits | GIE);
            __disable_interrupt();
        }
        g_tick_pending = false;
        __enable_interrupt();

        /* One conversion + readout covers both channels (~20 us). The mux
         * selection never changes, so there is no rotation to keep in step:
         * every access re-commands ADC_PAIR (see adc168_read()).
         *
         * This is the burst the scope sees: CONVST pulse, 24 clocks, BUSY
         * falling, RD pulse, 40 clocks with both results on SDOA. */
        switch (adc168_read(&va, &vb)) {
        case ADC168_OK:
            break;
        case ADC168_ERR_BUSY_TIMEOUT:
            g_err_busy++;
            va = vb = INT16_MIN;                /* -32768 = "invalid" marker */
            break;
        case ADC168_ERR_BAD_FRAME:
        default:
            g_err_frame++;
            va = vb = INT16_MIN;
            break;
        }

        g_sample_a = va;
        g_sample_b = vb;
        g_tick++;

        /* Heartbeat: toggle every 50 ticks = 1 Hz blink at ~100 Hz tick.
         * This is the "firmware is alive and ticking at the right rate"
         * signal — and, on a scope, P1.0 is a free 1 Hz timebase reference. */
        if ((g_tick % 50u) == 0u) {
            LED_HEART_TOGGLE();
        }
        /* Error LED: latched on by init trouble or any frame/BUSY error
         * that has ever occurred. Read g_status / g_err_* with a debugger to
         * find out which. */
        if (g_err_frame != 0u || g_err_busy != 0u) {
            LED_ERR_ON();
        }
    }
}
