#include <stdbool.h>
#include "board.h"
#include "clocks.h"
#include "uart.h"
#include "spi.h"
#include "adc168m102.h"

/*
 * =============================================================================
 *  Application: sample 2 ADC channels every ~10 ms, report over serial
 * =============================================================================
 *  The two channels are CHA<ADC_PAIR> and CHB<ADC_PAIR> (board.h; pair 1 by
 *  default = CHA1 on EVM J2.5 and CHB1 on EVM J1.5). They form one "pair", so
 *  the ADC's two converters digitize them at the SAME instant in a single
 *  conversion; the two results are then fetched by two read accesses.
 *
 *  Every tick that touches the ADC also emits one line on the backchannel
 *  UART — 115200-8-N-1 out through the eZ-FET as a USB CDC port on the host
 *  (the second /dev/ttyACM* on Linux). There are exactly two record types,
 *  and which one a tick produces is simply which phase the firmware is in:
 *
 *    C,<n>,<cfg>,<errs>     idle:      n-th CONFIG probe; cfg is the raw
 *                                      readback, 4 hex digits (expect 1041)
 *    D,<n>,<a>,<b>,<errs>   streaming: n-th sample; a = CHA<ADC_PAIR> and
 *                                      b = CHB<ADC_PAIR> as signed decimal,
 *                                      with -32768 marking a failed read
 *
 *  The leading tag is what lets a host parser keep the two apart in one
 *  stream, and <errs> is the running total of everything the firmware counts
 *  as an error — bad frames, BUSY timeouts, wrong readbacks, and lines the
 *  UART had to drop — so a stream sitting at 0 is one where nothing has gone
 *  wrong yet. Lines starting '#' are comments: the boot banner and the
 *  idle -> streaming transition.
 *
 *  The acquisition remains just as visible on the ADC bus itself, and that is
 *  still the only way to see the frames as they are shifted: SDOA carries the
 *  two 20-bit frames, one per read access. Trigger a scope on the CONVST
 *  rising edge — it fires once per tick with ~10 ms of quiet either side, so
 *  a single-shot capture lands on a whole acquisition every time.
 *  EXAMPLE_OUTPUTS.md shows the expected trace and walks a readout burst
 *  through to two numbers.
 *
 *  Health is reported on the two LaunchPad LEDs as well — the green one
 *  (LED2, P1.0) is the heartbeat, the red one (LED1, P4.6) the latched error
 *  light — and every value the firmware computes stays in a global that a
 *  debugger can read (mspdebug: `md &g_...`) — see the "observable state"
 *  block below.
 *
 *  Two run phases, and a LaunchPad button moves between them:
 *
 *    PHASE_IDLE   (entered at reset) Once a second, write the ADC CONFIG
 *                 register with the "read it back" action and read the reply
 *                 - one register write + one register read, nothing else on
 *                 the bus. This is the bring-up/probe mode: it proves the
 *                 link is alive at a rate a human can watch, and leaves the
 *                 analog front end idle. Each probe emits one 'C' record and
 *                 toggles the heartbeat LED -> a slow 0.5 Hz blink. Ticks in
 *                 between do nothing but poll the buttons, so the serial
 *                 output while idle is one line a second.
 *
 *    PHASE_STREAM (entered when S1 or S2 is pressed) The acquisition loop:
 *                 one conversion + two read accesses every tick, 100 Hz, and
 *                 one 'D' record per tick out of the UART.
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
 *  between ticks is a plain busy-wait on the flag the timer ISR raises. Three
 *  rates exist in the design, and SMCLK divides down to all of them: the
 *  16 MHz system clock itself, the 0.5 MHz ADC bus clock, and 115200 baud on
 *  the UART.
 *
 *  The 100 Hz tick runs in BOTH phases; only what a tick does changes. That
 *  keeps one timebase for everything: the sample rate, the 1 s idle period
 *  (IDLE_CONFIG_TICKS), and the button poll/debounce interval.
 *
 *  Timing budget per 10 ms tick: ~150 us of ADC bus traffic while streaming
 *  at the 0.5 MHz SCLK — three 24-clock bursts (~100 us once a second while
 *  idle, two bursts) — plus a few us of bookkeeping and number formatting.
 *  That is under 2 % of the tick; the remaining ~98 % is spent spinning on
 *  the tick flag.
 *
 *  The serial line does NOT come out of that budget. A ~25-byte record takes
 *  ~2.2 ms to shift out at 115200 baud — a fifth of the tick — so it is
 *  handed to uart.c's ring buffer and clocked out by the eUSCI TX interrupt
 *  in the background, during the spin. Nothing in the tick path ever blocks
 *  on the UART: if the ring were somehow still full, uart_write() drops the
 *  whole line and counts it rather than delaying the next sample.
 * =============================================================================
 */

/* ---- observable state ---------------------------------------------------
 * The serial stream carries the live values and one running error total;
 * these globals are where the individual counters live, so halting the target
 * with mspdebug and dumping them is how you find out WHICH error the total
 * has been counting. They are `volatile` so the compiler cannot fold away
 * stores nobody reads. */

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

/* ---- serial records -----------------------------------------------------
 * printf() from newlib would pull in several kilobytes of code and is far
 * too slow for the tick path, so the two record types are assembled by hand
 * from these three formatters. Each writes straight into the line buffer and
 * returns the advanced pointer, so a record is just a chain of calls. */

/* Unsigned 32-bit -> decimal ASCII, no leading zeros. */
static char *fmt_u32(char *p, uint32_t v)
{
    char tmp[10];                   /* max 4294967295 = 10 digits */
    uint8_t n = 0;

    do {                            /* peel digits least-significant first */
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {               /* ...then emit them reversed */
        *p++ = tmp[--n];
    }
    return p;
}

/* Signed 16-bit -> decimal ASCII with '-' sign. */
static char *fmt_i16(char *p, int16_t v)
{
    int32_t x = v;                  /* widen so negating -32768 can't overflow */

    if (x < 0) {
        *p++ = '-';
        x = -x;
    }
    return fmt_u32(p, (uint32_t)x);
}

/* Unsigned 16-bit -> exactly 4 uppercase hex digits (register readbacks). */
static char *fmt_hex16(char *p, uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    int8_t i;

    for (i = 12; i >= 0; i -= 4) {  /* nibbles from most to least significant */
        *p++ = hex[(v >> i) & 0xFu];
    }
    return p;
}

/* One shared scratch buffer rather than a local in each emitter. Only main()
 * context ever formats a record - the timer and UART ISRs do not - so there
 * is no reentrancy to worry about, and keeping it out of the frame keeps the
 * tick path's stack where board.h's inlining note measured it. Worst case is
 * "D,4294967295,-32768,-32768,4294967295\r\n" = 39 bytes. */
static char s_line[48];

/* Every error the firmware counts, as one number. Summed rather than reported
 * per-kind so a record stays short and a watcher has exactly one field to
 * check; the individual counters are the globals above, for a debugger. UART
 * drops are in here too - a dropped line is missing data like any other. */
static uint32_t err_total(void)
{
    return (uint32_t)g_err_frame + g_err_busy + g_err_cfg + uart_tx_drops();
}

/* "C,<probe>,<cfg hex>,<errs>" - the idle phase's once-a-second record. */
static void emit_config(uint16_t cfg)
{
    char *p = s_line;

    *p++ = 'C';
    *p++ = ',';
    p = fmt_u32(p, g_cfg_cycles);
    *p++ = ',';
    p = fmt_hex16(p, cfg);
    *p++ = ',';
    p = fmt_u32(p, err_total());
    *p++ = '\r';
    *p++ = '\n';
    (void)uart_write(s_line, (uint16_t)(p - s_line));
}

/* "D,<sample>,<a>,<b>,<errs>" - the streaming phase's per-tick record. */
static void emit_sample(int16_t a, int16_t b)
{
    char *p = s_line;

    *p++ = 'D';
    *p++ = ',';
    p = fmt_u32(p, g_tick);
    *p++ = ',';
    p = fmt_i16(p, a);
    *p++ = ',';
    p = fmt_i16(p, b);
    *p++ = ',';
    p = fmt_u32(p, err_total());
    *p++ = '\r';
    *p++ = '\n';
    (void)uart_write(s_line, (uint16_t)(p - s_line));
}

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
    char *p;                        /* banner formatting cursor            */

    /* Stop the watchdog timer. On MSP430 the WDT is RUNNING out of reset and
     * would reset the chip in ~32 ms unless serviced; we do not use it, so
     * hold it. WDTCTL is password-protected: the upper byte must be WDTPW
     * (0x5A) on every write, otherwise the chip resets immediately. WDTHOLD
     * is the "stop counting" bit. */
    WDTCTL = WDTPW | WDTHOLD;

    clock_init();                   /* GPIO map, I/O latch, DCO 16 MHz       */
    g_status = 0u;                  /* no crystal to fail -> nothing to flag */
    uart_init();                    /* 115200 baud on the backchannel        */
    spi_init();                     /* 0.5 MHz CPOL0/CPHA1 master            */
    __enable_interrupt();           /* set GIE: the UART TX ISR may now run  */

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

    /* Boot banner. '#' marks a comment line for whoever is parsing the
     * stream; the two record layouts are spelled out so a capture is
     * self-describing. status is the ST_* bits, cfg the same readback
     * published in g_cfg (expect 1041 when the wiring is right).
     *
     * uart_flush() blocks until the bytes have physically left the shift
     * register, which is exactly what must not happen once the tick is
     * running - but the timer has not been started yet, so here it costs
     * nothing. It is also what keeps the banner inside the 256-byte TX ring:
     * without it the later lines would arrive at a ring the first ones have
     * not drained out of, and uart_write() would drop them. */
    uart_puts("# adc168m102 fw  status=");
    p = fmt_hex16(s_line, g_status);
    *p++ = ' ';
    (void)uart_write(s_line, (uint16_t)(p - s_line));
    uart_puts("cfg=");
    p = fmt_hex16(s_line, g_cfg);
    *p++ = '\r';
    *p++ = '\n';
    (void)uart_write(s_line, (uint16_t)(p - s_line));
    uart_flush();
    uart_puts("# C,n,cfg,errs    idle: n-th CONFIG probe, cfg hex\r\n");
    uart_flush();
    uart_puts("# D,n,a,b,errs    stream: n-th sample, -32768 = bad read\r\n");
    uart_flush();
    uart_puts("# press S1 or S2 to start streaming\r\n");
    uart_flush();

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
                /* Mark the phase change in the stream so a capture shows
                 * where the 'C' records stop and the 'D' records begin.
                 * Non-blocking like every other tick-path write. */
                (void)uart_puts("# streaming\r\n");
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

            /* This tick's line out. Emitted after the error check so a bad
             * readback is already counted in the record's <errs> field. */
            emit_config(cfg);

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

        /* This tick's line out. The counters bumped by the switch above are
         * already folded into <errs>, so the record that reports a bad
         * conversion also reports the error it caused. */
        emit_sample(va, vb);

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
