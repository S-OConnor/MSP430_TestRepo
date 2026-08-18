#include "board.h"
#include "uart.h"

/*
 * =============================================================================
 *  eUSCI_A0 UART, 115200-8-N-1, interrupt-driven transmit
 * =============================================================================
 *  P2.0/UCA0TXD and P2.1/UCA0RXD are wired on the LaunchPad to the eZ-FET
 *  debug chip, which forwards them to the PC as a USB CDC serial port (the
 *  "backchannel UART" — normally the second /dev/ttyACM port).
 *
 *  TX is fully interrupt-driven through a ring buffer so the 100 Hz sample
 *  loop never blocks on the serial port: a full CSV line (~74 bytes) takes
 *  ~6.4 ms to shift out at 115200 baud, which would eat most of the 10 ms
 *  tick if written synchronously.
 * =============================================================================
 */

#define TX_RING_SIZE 256u           /* power of two so "& MASK" wraps indices */
#define TX_RING_MASK (TX_RING_SIZE - 1u)

static volatile uint8_t s_ring[TX_RING_SIZE];
static volatile uint16_t s_head;    /* write index, advanced by uart_write()  */
static volatile uint16_t s_tail;    /* read index, advanced by the TX ISR     */
static volatile uint16_t s_drops;   /* count of writes rejected for no space  */

void uart_init(void)
{
    /* UCSWRST = software reset. While it is 1 the module is held in reset
     * and its configuration registers may be written safely; the module
     * starts operating when the bit is cleared. Standard eUSCI init pattern:
     * set UCSWRST -> configure -> clear UCSWRST. */
    UCA0CTLW0 = UCSWRST;

    /* Bit-clock source select: BRCLK = SMCLK (8 MHz). (UART mode, 8N1, LSB
     * first are all the register's default = 0 settings, so only the clock
     * source needs setting.) */
    UCA0CTLW0 |= UCSSEL__SMCLK;

    /* Baud-rate generator, oversampling mode (SLAU367 user's guide method):
     *   N = BRCLK / baud = 8'000'000 / 115'200 = 69.44
     *   N >= 16, so use oversampling (UCOS16 = 1):
     *     UCBR  = floor(N/16)            = floor(4.34) = 4
     *     UCBRF = floor((N/16 - 4) * 16) = 5             (first mod stage)
     *     UCBRS = 0x55                                   (second mod stage,
     *             fractional-part lookup from the SLAU367 baud table)
     * UCA0MCTLW packs: UCBRS in the high byte, UCBRF in bits 7:4, UCOS16. */
    UCA0BRW = 4;
    UCA0MCTLW = 0x5500 | UCBRF_5 | UCOS16;

    /* Release the module from reset — the UART is now live. (The TX interrupt
     * is enabled on demand by uart_write, not here.) */
    UCA0CTLW0 &= ~UCSWRST;
}

/* Bytes of free space in the ring. One slot is deliberately never used so
 * that head == tail always means "empty" (and never "full"). */
static uint16_t ring_free(void)
{
    return (uint16_t)(TX_RING_SIZE - 1u - ((s_head - s_tail) & TX_RING_MASK));
}

bool uart_write(const char *buf, uint16_t len)
{
    uint16_t i;

    /* All-or-nothing admission: dropping a whole line keeps the CSV stream
     * parseable (a partially-enqueued line would splice into the next one). */
    if (len > ring_free()) {
        s_drops++;
        return false;
    }

    /* Copy the payload in BEFORE publishing the new head index: the ISR only
     * consumes up to s_head, so it can never read half-written bytes. */
    for (i = 0; i < len; i++) {
        s_ring[(s_head + i) & TX_RING_MASK] = (uint8_t)buf[i];
    }

    /* Publish and kick the transmitter. The two steps are done with
     * interrupts masked so the ISR cannot observe the head moving while we
     * are also flipping its enable bit.
     *
     * Restart subtlety: UCTXIFG ("TX buffer empty") is level-set whenever
     * UCA0TXBUF has room — including while the pipeline is idle. So simply
     * setting UCTXIE here immediately re-fires the ISR, which pulls the
     * first byte from the ring. No manual "prime the first byte" needed. */
    __disable_interrupt();
    s_head = (uint16_t)((s_head + len) & TX_RING_MASK);
    UCA0IE |= UCTXIE;
    __enable_interrupt();
    return true;
}

bool uart_puts(const char *s)
{
    uint16_t n = 0;
    while (s[n] != '\0') {          /* strlen, avoiding a libc dependency */
        n++;
    }
    return uart_write(s, n);
}

void uart_flush(void)
{
    /* Busy-wait until the ISR has drained the ring... */
    while (s_head != s_tail) {
        /* spin (interrupts must be enabled; only used for the boot banner) */
    }
    /* ...and until the final byte has fully left the shift register
     * (UCBUSY = 1 while a frame is still being transmitted). */
    while (UCA0STATW & UCBUSY) {
    }
}

uint16_t uart_tx_drops(void)
{
    return s_drops;
}

/* eUSCI_A0 interrupt. UCA0IV is a hardware-prioritized "interrupt vector"
 * register: reading it returns a code for the highest-pending source (0x02 =
 * RX full, 0x04 = TX empty, ...) AND clears that source's flag. The
 * __even_in_range() intrinsic just tells the compiler the value is even and
 * bounded so it can emit a compact jump table. */
void __attribute__((interrupt(USCI_A0_VECTOR))) usci_a0_isr(void)
{
    switch (__even_in_range(UCA0IV, USCI_UART_UCTXCPTIFG)) {
    case USCI_UART_UCTXIFG:                 /* TX buffer ready for a byte */
        if (s_tail != s_head) {
            /* Writing UCA0TXBUF clears UCTXIFG; it re-asserts (and re-enters
             * this ISR) when the byte moves into the shift register. */
            UCA0TXBUF = s_ring[s_tail];
            s_tail = (uint16_t)((s_tail + 1u) & TX_RING_MASK);
        } else {
            /* Ring empty: disable the TX interrupt, otherwise the level-set
             * UCTXIFG would re-enter this ISR forever. uart_write() re-arms
             * it when new data shows up. */
            UCA0IE &= ~UCTXIE;
        }
        break;
    default:                                /* RX and error sources unused */
        break;
    }
}
