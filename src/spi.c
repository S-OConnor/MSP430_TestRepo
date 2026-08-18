#include "board.h"
#include "spi.h"

/*
 * =============================================================================
 *  eUSCI_B0 as 3-wire SPI master for the ADC168M102R-SEP
 * =============================================================================
 *  The ADC's CLOCK pin is BOTH its conversion clock and its serial clock, and
 *  the datasheet (SBASAW9 §6.3.1.4) explicitly allows it to be gated: "keep
 *  the clock held static low or high when read access completes and before
 *  starting a new conversion". An SPI master is exactly such a gated clock —
 *  SCLK only toggles while bytes are in flight and idles at its CPOL level
 *  in between. So every spi_xfer() call is simultaneously:
 *    - 8 conversion clocks for the ADC's SAR state machine, and
 *    - 8 bit-slots of serial data on SDI (MOSI) / SDOA (MISO).
 *
 *  Required timing relationship (half-clock mode):
 *    - the ADC updates SDOA on CLOCK RISING edges  -> master samples FALLING
 *    - the ADC latches SDI on CLOCK FALLING edges  -> master shifts on RISING
 *    - CLOCK idles low between bursts
 *  In Motorola terms that is CPOL=0 / CPHA=1. In eUSCI terms:
 *    UCCKPL = 0  (clock inactive state low = CPOL 0)
 *    UCCKPH = 0  (data CHANGED on the first edge, captured on the second —
 *                 note TI's UCCKPH is the INVERSE of Motorola CPHA)
 * =============================================================================
 */

void spi_init(void)
{
    /* Hold the module in software reset while configuring (same pattern as
     * the UART: registers are only safely writable under UCSWRST=1). */
    UCB0CTLW0 = UCSWRST;

    /* Configuration bits OR'd into the control word:
     *   UCMST        master mode (we generate SCLK)
     *   UCSYNC       synchronous mode (SPI rather than I2C/UART)
     *   UCMSB        MSB-first bit order (the ADC shifts MSB first)
     *   UCSSEL__SMCLK bit-clock source = SMCLK (8 MHz)
     *   UCCKPH=0, UCCKPL=0 (not set) -> CPOL=0/CPHA=1 as explained above.
     *   UCMODE_0 (not set) -> 3-pin SPI: no STE line, ~CS is a plain GPIO. */
    UCB0CTLW0 |= UCMST | UCSYNC | UCMSB | UCSSEL__SMCLK;

    /* Bit-rate divider: SCLK = SMCLK / UCB0BRW. With the default divider of
     * 1 that is 8 MHz (period 125 ns) — inside the ADC's 0.5..20 MHz
     * half-clock window. See ADC_SCLK_DIV in board.h for the slow-down knob. */
    UCB0BRW = ADC_SCLK_DIV;

    /* Release from reset: SCLK now sits idle-low, ready to burst. */
    UCB0CTLW0 &= ~UCSWRST;
}

uint8_t spi_xfer(uint8_t tx)
{
    /* Wait until the transmit buffer can take a byte. UCTXIFG is set by
     * hardware whenever UCB0TXBUF is empty. */
    while (!(UCB0IFG & UCTXIFG)) {
    }

    /* Writing the TX buffer starts the 8-clock burst. SPI is full duplex:
     * while these 8 bits shift out on SIMO, 8 bits shift in from SOMI. */
    UCB0TXBUF = tx;

    /* UCRXIFG sets when the 8th bit has been captured and the received byte
     * has moved into UCB0RXBUF — i.e. the burst is complete and SCLK is
     * guaranteed to be back at idle-low. ~1 us at 8 MHz, so a blocking wait
     * is simpler and cheaper than interrupt plumbing here. */
    while (!(UCB0IFG & UCRXIFG)) {
    }

    /* Reading UCB0RXBUF clears UCRXIFG automatically. */
    return UCB0RXBUF;
}
