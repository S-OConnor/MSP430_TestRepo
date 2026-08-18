#include <stdbool.h>
#include "board.h"
#include "clocks.h"
#include "uart.h"
#include "spi.h"
#include "adc168m102.h"

/*
 * =============================================================================
 *  Application: scan 8 ADC channels every ~10 ms, stream them as CSV text
 * =============================================================================
 *  Control flow:
 *    1. Hardware init (watchdog off, clocks, UART, SPI, ADC configuration).
 *    2. Print a banner with the status flags, then start the tick timer.
 *    3. Loop forever: sleep in LPM0 -> timer interrupt wakes us -> read the
 *       four channel pairs -> format one CSV line -> hand it to the UART ring
 *       buffer (non-blocking) -> back to sleep.
 *
 *  Timing budget per 10 ms tick: ~80 us of ADC traffic, ~50 us of number
 *  formatting; the UART then shifts the ~74-byte line out in the background
 *  (~6.4 ms at 115200 baud) while the CPU sleeps.
 * =============================================================================
 */

/* Set by the timer ISR, consumed by main(). `volatile` because it is written
 * in interrupt context and read in a loop the compiler would otherwise hoist. */
static volatile bool g_tick_pending;

static uint32_t g_tick;             /* sample counter, first column of CSV   */
static uint16_t g_err_frame;        /* frames that failed indicator-bit check */
static uint16_t g_err_busy;         /* conversions where BUSY never dropped   */
static uint8_t g_status;            /* ST_* flags from init                   */

/* ---- tiny number formatters ---------------------------------------------
 * printf() from newlib would pull in several kilobytes and is far too slow
 * for the tick path; these write straight into the line buffer and return
 * the advanced pointer. */

/* Unsigned 32-bit -> decimal ASCII (no leading zeros). */
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

/* Unsigned 16-bit -> 4 uppercase hex digits (banner only). */
static char *fmt_hex16(char *p, uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    int8_t i;

    for (i = 12; i >= 0; i -= 4) {  /* nibbles from most to least significant */
        *p++ = hex[(v >> i) & 0xFu];
    }
    return p;
}

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

/* ---- optional SPI loopback self-test (build with -DSPI_LOOPBACK_TEST=ON) --
 * With P1.6 (SIMO) jumpered to P1.7 (SOMI) and NO ADC attached, every byte
 * we send must come straight back. Proves the eUSCI setup, pin muxing, and
 * clock phase independent of the ADC. Never returns. */
#ifdef SPI_LOOPBACK_TEST
static void spi_loopback_test(void)
{
    static const uint8_t pat[] = { 0x00, 0xFF, 0x55, 0xAA, 0xA5, 0x0F };
    uint16_t i;
    bool pass = true;

    for (i = 0; i < 1000u; i++) {
        uint8_t tx = pat[i % (sizeof pat)] ^ (uint8_t)i;    /* varied bytes */
        if (spi_xfer(tx) != tx) {
            pass = false;
            break;
        }
    }
    uart_puts(pass ? "# SPI loopback PASS\r\n" : "# SPI loopback FAIL\r\n");
    uart_flush();
    for (;;) {                      /* blink code: slow = PASS, fast = FAIL */
        LED_HEART_TOGGLE();
        if (pass) {
            __delay_cycles(MCLK_HZ / 2uL);
        } else {
            __delay_cycles(MCLK_HZ / 10uL);
        }
    }
}
#endif

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    /* Latest scan. Index k = channel pair: va[k] = CHAk, vb[k] = CHBk, and
     * the two were sampled at the same instant. */
    int16_t va[4], vb[4];
    char line[96];                  /* worst-case CSV line is ~74 bytes */
    char *p;
    uint8_t k;

    /* Stop the watchdog timer. On MSP430 the WDT is RUNNING out of reset and
     * would reset the chip in ~32 ms unless serviced; we do not use it, so
     * hold it. WDTCTL is password-protected: the upper byte must be WDTPW
     * (0x5A) on every write, otherwise the chip resets immediately. WDTHOLD
     * is the "stop counting" bit. */
    WDTCTL = WDTPW | WDTHOLD;

    g_status = clock_init();        /* GPIO map, LPM5 unlock, 16/8 MHz, LFXT */
    uart_init();                    /* 115200 baud on the backchannel        */
    spi_init();                     /* 8 MHz CPOL0/CPHA1 master              */
    __enable_interrupt();           /* set GIE: UART TX ISR may now run      */

#ifdef SPI_LOOPBACK_TEST
    spi_loopback_test();            /* does not return */
#endif

    /* Reset + configure the ADC; also runs the link check. */
    g_status |= adc168_init();

    /* Boot banner. '#'-prefixed lines are comments for whoever parses the
     * CSV. status: ST_* bits; cfg: raw CONFIG readback from the link check
     * (expect 0x1041 when the wiring is right). uart_flush() here is fine —
     * the tick timer is not running yet, so blocking costs nothing. */
    uart_puts("# adc168m102 fw v0.1 status=0x");
    p = fmt_hex16(line, g_status);
    *p++ = ' ';
    uart_write(line, (uint16_t)(p - line));
    uart_puts("cfg=0x");
    p = fmt_hex16(line, adc168_link_readback());
    *p++ = '\r';
    *p++ = '\n';
    uart_write(line, (uint16_t)(p - line));
    uart_puts("# tick,a0,a1,a2,a3,b0,b1,b2,b3,errs\r\n");
    uart_flush();

    timer_init(g_status);           /* start the ~100 Hz tick */

    for (;;) {
        /* Sleep until the tick ISR sets the flag. The disable/test/sleep
         * sequence avoids the classic race where the interrupt fires between
         * "test flag" and "go to sleep" and we then sleep through a tick:
         * __bis_SR_register(LPM0_bits | GIE) atomically re-enables interrupts
         * AND enters low-power mode 0 in one instruction.
         *
         * LPM0 (not the deeper LPM3) because LPM0 keeps SMCLK running, and
         * the UART TX interrupt still needs SMCLK to shift out the previous
         * line while we sleep. */
        __disable_interrupt();
        while (!g_tick_pending) {
            __bis_SR_register(LPM0_bits | GIE);
            __disable_interrupt();
        }
        g_tick_pending = false;
        __enable_interrupt();
        g_tick++;

        /* Scan all 8 channels as 4 pair conversions (~80 us total).
         *
         * Pipeline bookkeeping: the pair converted NOW was selected by the
         * command sent during the PREVIOUS readout. Iteration k therefore
         * converts pair k and asks for pair (k+1)&3 next — so after k=3 the
         * ADC is already primed for pair 0 at the start of the next tick. */
        for (k = 0; k < 4u; k++) {
            switch (adc168_read_pair((uint8_t)((k + 1u) & 3u), &va[k], &vb[k])) {
            case ADC168_OK:
                break;
            case ADC168_ERR_BUSY_TIMEOUT:
                g_err_busy++;
                va[k] = vb[k] = INT16_MIN;      /* -32768 = "invalid" marker */
                break;
            case ADC168_ERR_BAD_FRAME:
            default:
                g_err_frame++;
                va[k] = vb[k] = INT16_MIN;
                break;
            }
        }

        /* Format: tick,a0,a1,a2,a3,b0,b1,b2,b3,errs\r\n */
        p = line;
        p = fmt_u32(p, g_tick);
        for (k = 0; k < 4u; k++) {
            *p++ = ',';
            p = fmt_i16(p, va[k]);
        }
        for (k = 0; k < 4u; k++) {
            *p++ = ',';
            p = fmt_i16(p, vb[k]);
        }
        *p++ = ',';
        p = fmt_u32(p, (uint32_t)g_err_frame + g_err_busy + uart_tx_drops());
        *p++ = '\r';
        *p++ = '\n';

        /* Non-blocking hand-off to the UART ring; if the ring is somehow
         * still full (host stalled?), the whole line is dropped and counted
         * rather than delaying the next sample. */
        (void)uart_write(line, (uint16_t)(p - line));

        /* Heartbeat: toggle every 50 ticks = 1 Hz blink at ~100 Hz tick. */
        if ((g_tick % 50u) == 0u) {
            LED_HEART_TOGGLE();
        }
        /* Error LED: latched on if init flagged anything or any frame/BUSY
         * error has ever occurred. */
        if (g_status != 0u || g_err_frame != 0u || g_err_busy != 0u) {
            LED_ERR_ON();
        } else {
            LED_ERR_OFF();
        }
    }
}
