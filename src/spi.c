#include <stddef.h>
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
 *  in between. So every burst is simultaneously:
 *    - N*8 conversion clocks for the ADC's SAR state machine, and
 *    - N*8 bit-slots of serial data on SDI (MOSI) / SDOA (MISO).
 *
 *  Required timing relationship (half-clock mode):
 *    - the ADC updates SDOA on CLOCK RISING edges  -> master samples FALLING
 *    - the ADC latches SDI on CLOCK FALLING edges  -> master shifts on RISING
 *    - CLOCK idles low between bursts
 *  In Motorola terms that is CPOL=0 / CPHA=1. In eUSCI terms:
 *    UCCKPL = 0  (clock inactive state low = CPOL 0)
 *    UCCKPH = 0  (data CHANGED on the first edge, captured on the second —
 *                 note TI's UCCKPH is the INVERSE of Motorola CPHA)
 *
 *  --- Why this module deals in BURSTS, not single bytes --------------------
 *
 *  "Gated clock is allowed" is about the quiet BETWEEN accesses. Inside one
 *  access the ADC is counting clock edges, and the strobes are specified
 *  against them: RD is captured on a CLOCK falling edge, CONVST wants the
 *  conversion to start on the next CLOCK rising edge, and a register write
 *  activates "with the CLOCK rising edge after completing the 16-clock write
 *  access". So the two things that matter are:
 *
 *    1. an access should be ONE contiguous train of clocks, and
 *    2. the strobe that opens it should sit immediately before the first
 *       edge, with as little and as predictable a gap as possible.
 *
 *  A naive byte-at-a-time routine gets both wrong, because it waits for
 *  UCRXIFG — "the byte has finished shifting" — before returning. The shift
 *  register is then EMPTY while the CPU returns, is called again, tests
 *  UCTXIFG and writes the next byte, so SCLK stalls at every byte boundary
 *  for as long as that round trip takes (tens of CPU cycles; around a
 *  microsecond, which at the 0.5 MHz bit clock is a visible fraction of a bit
 *  time). The same round trip sits between a strobe and the first clock edge.
 *
 *  spi_burst() fixes both by using the eUSCI's DOUBLE BUFFERING: it reloads
 *  UCB0TXBUF as soon as UCTXIFG says the buffer is free — which happens the
 *  moment the previous byte moves into the shift register, i.e. while that
 *  byte is still going out — so the next byte is already queued when the
 *  current one ends and the clock never stops mid-access. spi_wait_ready()
 *  covers the other half: call it BEFORE a strobe and the burst that follows
 *  starts clocking within a few cycles of the strobe, every time.
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
     *   UCSSEL__SMCLK bit-clock source = SMCLK (16 MHz, divided below)
     *   UCCKPH=0, UCCKPL=0 (not set) -> CPOL=0/CPHA=1 as explained above.
     *   UCMODE_0 (not set) -> 3-pin SPI: no STE line, ~CS is a plain GPIO. */
    UCB0CTLW0 |= UCMST | UCSYNC | UCMSB | UCSSEL__SMCLK;

    /* Bit-rate divider: SCLK = SMCLK / UCB0BRW. ADC_SCLK_DIV is 32, so
     * 16 MHz / 32 = 0.5 MHz (period 2 us) — the bottom end of the ADC's
     * 0.5..20 MHz half-clock window (SBASAW9 §6.3.1.4). Running at the slow
     * end maximises setup/hold margin on the jumper wires to the EVM; the
     * cost is bus time, ~150 us per tick, still far inside the 10 ms budget.
     * See ADC_SCLK_DIV in board.h to change the rate. */
    UCB0BRW = ADC_SCLK_DIV;

    /* Release from reset: SCLK now sits idle-low, ready to burst. */
    UCB0CTLW0 &= ~UCSWRST;
}

void spi_wait_ready(void)
{
    /* Two different conditions, and both are needed.
     *
     * UCBUSY (in UCB0STATW) is the one that means "the shift register has
     * drained and SCLK is parked at idle low". UCTXIFG alone does NOT mean
     * that: it sets the moment a queued byte moves from UCB0TXBUF into the
     * shift register, which is while that byte is still being clocked out.
     * A strobe fired on UCTXIFG alone could therefore land in the MIDDLE of
     * a burst.
     *
     * UCTXIFG is then what guarantees the write in spi_burst() will not
     * block, so the caller's strobe is followed by the first clock edge
     * after a fixed handful of instructions rather than after an
     * unpredictable wait. */
    while (UCB0STATW & UCBUSY) {
    }
    while (!(UCB0IFG & UCTXIFG)) {
    }
}

void spi_burst(const uint8_t *tx, uint8_t *rx, uint8_t n)
{
    uint8_t sent = 0u;
    uint8_t got = 0u;

    while (got < n) {
        /* Keep UCB0TXBUF fed. Loading on UCTXIFG — not on "previous byte
         * finished" — is the whole point: the byte written here slides into
         * the shift register the instant the current one ends, so the clock
         * runs straight through the byte boundary and the access is one
         * unbroken train of edges. TX is serviced first for that reason;
         * draining RX can wait, feeding TX cannot. */
        if ((sent < n) && (UCB0IFG & UCTXIFG)) {
            UCB0TXBUF = tx[sent];
            sent++;
            continue;
        }

        /* UCB0RXBUF has to be emptied before the NEXT byte finishes shifting
         * or it is overwritten (UCOE). That deadline is one byte time —
         * 16 us at the 0.5 MHz bit clock — against a 16 MHz CPU that has
         * nothing else to do, so it is never close. Reading the register is
         * itself what clears UCRXIFG. */
        if (UCB0IFG & UCRXIFG) {
            uint8_t d = UCB0RXBUF;

            if (rx != NULL) {
                rx[got] = d;
            }
            got++;
        }
    }

    /* Every byte has been received, so the shift register is empty and SCLK
     * is back at idle low: the caller may move a strobe as soon as this
     * returns. */
}
