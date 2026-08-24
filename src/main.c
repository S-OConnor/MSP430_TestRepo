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
 *  conversion; the two results are then fetched by two read accesses.
 *
 *  There is no serial output. The results are read off the ADC bus itself
 *  with a scope or logic analyzer: SDOA carries the two 20-bit frames, one per
 *  read access. Trigger on the CONVST rising edge — it fires once per tick
 *  with ~10 ms of quiet either side, so a single-shot capture lands on a whole
 *  acquisition every time. EXAMPLE_OUTPUTS.md shows the expected trace and
 *  walks a readout burst through to two numbers.
 *
 *  Health is reported on the two LaunchPad LEDs — the green one (LED2, P1.0)
 *  is the heartbeat, the red one (LED1, P4.6) the latched error light — and
 *  every value the firmware computes stays in a global that a debugger can
 *  read (mspdebug: `md &g_...`) — see the "observable state" block below.
 *
 *  Two run phases, and a LaunchPad button moves between them:
 *
 *    PHASE_IDLE   (entered at reset) Once a second, write the ADC CONFIG
 *                 register with the "read it back" action and read the reply
 *                 - one register write + one register read, nothing else on
 *                 the bus. This is the bring-up/probe mode: it proves the
 *                 link is alive at a rate a human can watch, and leaves the
 *                 analog front end idle. The heartbeat LED toggles once per
 *                 probe -> a slow 0.5 Hz blink.
 *
 *    PHASE_STREAM (entered when S1 or S2 is pressed) The acquisition loop:
 *                 one conversion + two read accesses every tick, 100 Hz.
 *                 Heartbeat toggles every 50 ticks -> a 1 Hz blink, visibly
 *                 twice the idle rate, so the LED alone tells you which
 *                 phase the board is in.
 *
 *  The switch is ONE WAY: once streaming, a further press does nothing (a
 *  reset returns to idle). Both buttons do the same thing - S1 = P4.5,
 *  S2 = P1.1, active low, polled every tick, see board.h.
 *
 *  Control flow:
 *    1. Hardware init (watchdog off, clocks, SPI, ADC configuration).
 *    2. Start the tick timer.
 *    3. Loop forever: spin on the tick flag -> timer interrupt sets it -> do
 *       this tick's work for the current phase -> update LEDs -> spin again.
 *
 *  The CPU runs continuously at 16 MHz and is never put to sleep: the wait
 *  between ticks is a plain busy-wait on the flag the timer ISR raises. The
 *  only two rates in the design are that 16 MHz system clock and the 0.5 MHz
 *  ADC bus clock.
 *
 *  The 100 Hz tick runs in BOTH phases; only what a tick does changes. That
 *  keeps one timebase for everything: the sample rate, the 1 s idle period
 *  (IDLE_CONFIG_TICKS), and the button poll/debounce interval.
 *
 *  Timing budget per 10 ms tick: ~150 us of ADC bus traffic while streaming
 *  at the 0.5 MHz SCLK — three 24-clock bursts (~100 us once a second while
 *  idle, two bursts) — plus a few us of bookkeeping. That is under 2 % of the
 *  tick; the remaining ~98 % is spent spinning on the tick flag.
 * =============================================================================
 */

/* ---- observable state ---------------------------------------------------
 * With the UART gone these globals ARE the readout path for anything the
 * scope cannot show: halt the target with mspdebug and dump them. They are
 * `volatile` so the compiler cannot fold away stores nobody reads. */

static volatile int16_t g_sample_a;     /* latest CHA<ADC_PAIR> code          */
static volatile int16_t g_sample_b;     /* latest CHB<ADC_PAIR> code          */
static volatile uint32_t g_tick;        /* samples since streaming started    */
static volatile uint16_t g_err_frame;   /* frames that failed the bit check   */
static volatile uint16_t g_err_busy;    /* conversions where BUSY never fell  */
static volatile uint8_t g_status;       /* ST_* flags from init               */
static volatile uint16_t g_cfg;         /* raw CONFIG readback (link check)   */
static volatile uint16_t g_cfg_cycles;  /* idle-phase config probes done      */
static volatile uint16_t g_err_cfg;     /* probes whose readback was wrong    */
static volatile uint8_t g_phase;        /* PHASE_IDLE / PHASE_STREAM          */

/* Set by the timer ISR, consumed by main(). `volatile` because it is written
 * in interrupt context and read in a busy-wait loop the compiler would
 * otherwise hoist out and spin on forever. */
static volatile bool g_tick_pending;

/* ---- run phases ---------------------------------------------------------
 * Held in g_phase so a debugger can see which mode the board is in without
 * timing the LED. */

#define PHASE_IDLE      0u      /* config write+readback once a second       */
#define PHASE_STREAM    1u      /* conversion + readout every tick           */

/* ---- button poll --------------------------------------------------------
 * Called once per tick (every ~10 ms) from the idle phase. Returns true on
 * the tick where a press becomes CONFIRMED - the pin has read low for
 * BTN_DEBOUNCE_POLLS consecutive polls, i.e. through the contact bounce.
 *
 * Polling beats a port interrupt here: the CPU is running anyway and already
 * visits this code every 10 ms, so the poll is free, and the tick spacing IS
 * the debounce - no extra timer, no ISR that could fire mid-bounce a dozen
 * times.
 *
 * The counter saturates rather than wrapping, so holding a button down keeps
 * returning true; that is harmless because the only caller leaves the idle
 * phase on the first true and never asks again. */
static bool button_pressed(void)
{
    static uint8_t down_polls;

    if (!BTN_ANY_DOWN()) {
        down_polls = 0;             /* released (or bounced high): restart */
        return false;
    }
    if (down_polls < BTN_DEBOUNCE_POLLS) {
        down_polls++;
    }
    return (down_polls >= BTN_DEBOUNCE_POLLS);
}

/* ---- sample tick timer -------------------------------------------------- */

static void timer_init(void)
{
    /* Timer_A0 in "up mode": the 16-bit counter climbs from 0 to TA0CCR0,
     * fires the CCR0 interrupt, resets to 0, and repeats. So the period is
     * (CCR0 + 1) timer clocks — hence the "- 1" on the constant.
     *
     * With no crystal on the board the timebase is SMCLK, i.e. the DCO:
     *   TASSEL__SMCLK  source = SMCLK (16 MHz)
     *   ID__8          input divider /8 -> 2 MHz timer clock
     *   MC__UP         up mode
     *   TACLR          clear the counter/divider state on start
     * 2 MHz / 20000 = exactly 100.000 Hz, to DCO accuracy (~+-2 %). The tick
     * only has to space the ADC bursts evenly — nothing measures absolute
     * time from it — so DCO accuracy is ample. */
    TA0CCR0 = TICK_PERIOD_SMCLK - 1u;
    TA0CCTL0 = CCIE;                /* enable the CCR0 compare interrupt */
    TA0CTL = TASSEL__SMCLK | ID__8 | MC__UP | TACLR;
}

/* Timer_A0 CCR0 interrupt: fires once per sample period. It does the absolute
 * minimum — raise a flag — so all the real work runs at normal priority in
 * main() with interrupts enabled. The CPU is already running (it is spinning
 * on that flag in main()), so there is no mode to leave on the way out: the
 * ISR simply returns and the next pass of the wait loop sees the flag. */
void __attribute__((interrupt(TIMER0_A0_VECTOR))) ta0_ccr0_isr(void)
{
    g_tick_pending = true;
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    int16_t va, vb;                 /* this tick's pair, before publishing */
    uint16_t idle_ticks = 0;        /* ticks since the last config probe   */
    uint16_t cfg;                   /* this probe's raw CONFIG readback    */

    /* Stop the watchdog timer. On MSP430 the WDT is RUNNING out of reset and
     * would reset the chip in ~32 ms unless serviced; we do not use it, so
     * hold it. WDTCTL is password-protected: the upper byte must be WDTPW
     * (0x5A) on every write, otherwise the chip resets immediately. WDTHOLD
     * is the "stop counting" bit. */
    WDTCTL = WDTPW | WDTHOLD;

    clock_init();                   /* GPIO map, I/O latch, DCO 16 MHz       */
    g_status = 0u;                  /* no crystal to fail -> nothing to flag */
    spi_init();                     /* 0.5 MHz CPOL0/CPHA1 master            */
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

    /* Start in the probe phase and wait for a human. Nothing converts until
     * S1 or S2 is pressed, so the analog front end can be probed, jumpered,
     * or left unpowered while the digital link is verified once a second. */
    g_phase = PHASE_IDLE;

    timer_init();                   /* start the 100 Hz tick */

    for (;;) {
        /* Busy-wait for the tick. The CPU stays running at 16 MHz with
         * interrupts enabled; the timer ISR raises g_tick_pending and this
         * loop falls through on its next pass. g_tick_pending is `volatile`,
         * so the compiler must re-read it every time round rather than
         * hoisting the test out of the loop.
         *
         * Clearing the flag is a single byte write and cannot tear, and a
         * tick that arrives while the work below is running simply leaves the
         * flag set, so the next pass through here does not wait — no
         * interrupt-disable window is needed anywhere. */
        while (!g_tick_pending) {
        }
        g_tick_pending = false;

        /* ---- idle phase: probe the CONFIG register, watch the buttons ----
         * Everything here runs at the same 100 Hz tick as streaming; the
         * button is looked at on every one of them and the ADC is touched
         * on every hundredth. */
        if (g_phase == PHASE_IDLE) {
            if (button_pressed()) {
                /* Hand over to acquisition. The probes above left C=00 in
                 * CONFIG, so this rewrite (and the two flush conversions it
                 * does) is what puts the mux on ADC_PAIR in time for the very
                 * next tick's readout. */
                adc168_start_stream();
                g_phase = PHASE_STREAM;
                g_tick = 0;             /* tick counter now means "samples" */
                continue;               /* first sample lands on the next tick */
            }

            if (++idle_ticks < IDLE_CONFIG_TICKS) {
                continue;               /* not a probe tick - wait for the next */
            }
            idle_ticks = 0;

            /* One write + one read of the CONFIG register (~100 us of bus
             * traffic). On a scope: two RD pulses, 24 clocks each, once a
             * second - and a stuck or mis-wired SDOA shows up immediately as
             * a bad readback rather than as bad samples later. */
            cfg = adc168_config_cycle();
            g_cfg = cfg;
            g_cfg_cycles++;
            if (!adc168_config_ok(cfg)) {
                g_err_cfg++;
                LED_ERR_ON();
            }

            /* Heartbeat at the probe rate: one toggle per second = 0.5 Hz
             * blink, half the streaming rate. The LED is the phase
             * indicator. */
            LED_HEART_TOGGLE();
            continue;
        }

        /* ---- streaming phase --------------------------------------------
         * One conversion + two read accesses covers both channels (~150 us).
         * The mux selection never changes, so there is no rotation to keep in
         * step: every access re-commands ADC_PAIR (see adc168_read()).
         *
         * This is the burst the scope sees: CONVST pulse, 24 clocks, BUSY
         * falling, then two read accesses — RD pulse plus 24 clocks each,
         * frame A then frame B on SDOA. */
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

        /* Heartbeat: toggle every 50 ticks = 1 Hz blink at ~100 Hz tick —
         * twice the idle-phase rate, so the LED also says which phase this
         * is. It is the "firmware is alive and ticking at the right rate"
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
