#include "board.h"
#include "spi.h"
#include "adc168m102.h"

/*
 * =============================================================================
 *  ADC168M102R-SEP driver — how this device actually talks
 * =============================================================================
 *  (All section/table references: TI datasheet SBASAW9, in docs/.)
 *
 *  The part contains TWO 16-bit SAR converters (A and B) that sample at the
 *  same instant. In pseudo-differential mode (PDE=1) each converter sits
 *  behind a 4:1 input multiplexer, so a "pair k" conversion (k = 0..3)
 *  digitizes CHAk and CHBk together — four conversions cover all 8 channels.
 *
 *  Unlike a normal SPI slave, the interface is built from five strobes/lines
 *  around the clock:
 *
 *    CLOCK   runs BOTH the SAR conversion and the data shifting. We supply it
 *            in gated bursts from the SPI peripheral (allowed per §6.3.1.4).
 *    CONVST  rising edge: sample-and-holds freeze; conversion starts on the
 *            next CLOCK rising edge and takes ~18 CLOCKs (half-clock mode).
 *    BUSY    high while the conversion runs.
 *    RD      falling edge: the ADC starts driving result bits on SDOA; it
 *            also opens a 16-CLOCK window during which whatever appears on
 *            SDI is latched as a command/register word.
 *    ~CS     gate for SDI/RD/SDOA; held low for the whole session.
 *
 *  Mode strapping (EVM header J5): M0 pin strapped to GND, M1 left pulled
 *  high -> "Mode II" = channel pair chosen manually by a command word, data
 *  on SDOA only. We additionally set the SR ("special read") config bit, so
 *  ONE RD pulse followed by 40 CLOCKs streams converter A's frame and then
 *  converter B's frame back-to-back on SDOA.
 *
 *  Frame format (CID=0, 20 clocks per converter):
 *
 *    bit:   19   18   17..2              1..0
 *           0    A/B  16-bit result      0 0
 *           |    |    (two's complement, |
 *           |    |     MSB first)        +-- padding zeros
 *           |    +-- converter indicator: 0 = ADC A, 1 = ADC B
 *           +-- constant leading zero
 *
 *  Those fixed indicator/zero bits are checked on every frame — a cheap,
 *  always-on integrity test of the wiring and clock phase.
 *
 *  Command word ("CONFIG register", Table 7-2), 16 bits MSB first:
 *
 *    15 14 | 13 12 | 11 10 | 9  | 8  | 7  | 6   | 5   | 4  | 3..0
 *    C[1:0]| R[1:0]| PD    | FE | SR | FC | PDE | CID | CE | A[3:0]
 *
 *    C   next channel pair       R   write mode (00 = update C only,
 *    PD  power-down control          01 = rewrite whole CONFIG)
 *    FE  FIFO enable             SR  special read (both frames per RD)
 *    FC  full-clock mode         PDE pseudo-differential enable
 *    CID channel-ID disable      CE  2-bit counter enable
 *    A   action/address: 0000 none, 0001 read CONFIG back, 0100 soft reset,
 *        x010/x101 "next word goes to REFDAC1/2", 1100 "... to REFCM", ...
 * =============================================================================
 */

static uint16_t s_link_readback;    /* raw CONFIG readback, shown in banner */

/*
 * Write one 16-bit word into the ADC.
 *
 * The RD pulse opens the SDI latch window; the next 16 CLOCK falling edges
 * (= our first two SPI bytes) clock the word in, MSB first.
 *
 * The third, dummy byte matters: the datasheet says a register update only
 * becomes ACTIVE "with the CLOCK rising edge after completing the 16-clock
 * write access" (§7). With a free-running clock that edge arrives naturally;
 * with our gated clock it would never come until the next access — so we
 * append 8 extra clocks to deliver it immediately.
 */
static void write_word(uint16_t w)
{
    ADC_RD_PULSE();
    (void)spi_xfer((uint8_t)(w >> 8));      /* bits 15..8 */
    (void)spi_xfer((uint8_t)w);             /* bits  7..0 */
    (void)spi_xfer(0x00);                   /* activation clock edges */
}

/*
 * Read one 16-bit register back (only used for the init link check, while
 * SR is still 0 so the reply is a single 20-clock frame).
 *
 * After a "read register" command (A=0001 etc.), the ADC presents the value
 * during the NEXT access: RD pulse, then the frame arrives as
 *   [2 indicator bits][16 register bits][trailing 00]
 * spread over our 3 bytes (24 clocks; the last 4 are padding).
 * Bit surgery below: drop the 2 indicator bits, keep the middle 16.
 */
static uint16_t read_word(void)
{
    uint8_t b0, b1, b2;

    ADC_RD_PULSE();
    b0 = spi_xfer(0x00);                    /* [ind ind d15..d10] */
    b1 = spi_xfer(0x00);                    /* [d9 .. d2]         */
    b2 = spi_xfer(0x00);                    /* [d1 d0 0 0 x x x x]*/
    return (uint16_t)(((uint16_t)(b0 & 0x3F) << 10) |   /* d15..d10 */
                      ((uint16_t)b1 << 2) |             /* d9..d2   */
                      ((uint16_t)b2 >> 6));             /* d1..d0   */
}

uint8_t adc168_init(void)
{
    uint8_t status = 0;
    int16_t d0, d1;
    uint8_t i;

    /* Assert ~CS once and leave it low: SDI/RD become live, SDOA drives. */
    ADC_CS_LOW();

    /* Software reset (A=0100): all registers to power-up defaults, any
     * ongoing conversion aborted. Interface is usable again ~20 ns later —
     * far shorter than the gap to our next SPI byte. */
    write_word(ADC168_W_RESET);

    /* --- Link check ------------------------------------------------------
     * Prove the SPI wiring/phase and the RD strobe work before touching the
     * real configuration: write CONFIG with PDE=1 plus the "read CONFIG
     * back" action (A=0001), then fetch the reply and compare the mode bits
     * we just wrote (bits 11:4 must read PD=00,FE=0,SR=0,FC=0,PDE=1,CID=0,
     * CE=0 = 0x04). Done while SR is still 0 so the reply framing is the
     * simple single-frame kind. A dead MISO line (0x0000/0xFFFF) fails this. */
    write_word(ADC168_W_LINKCHK);
    s_link_readback = read_word();
    if (((s_link_readback >> 4) & 0xFFu) != 0x04u) {
        status |= ST_ADC_NOLINK;
    }

    /* Real operating configuration: R=01 (rewrite whole register),
     * SR=1 (both frames per RD pulse), PDE=1 (8 pseudo-diff channels),
     * CID=0 (keep indicator bits — our per-frame validation), C=00
     * (first conversion will be pair 0). */
    write_word(ADC168_W_CONFIG);

    /* --- Reference bring-up ----------------------------------------------
     * The two internal 2.5 V reference DACs power up DISABLED (REFDACx
     * default 0x07FF has the power-down bit set). Writing an "address"
     * word (A=x010 / x101) makes the FOLLOWING word land in that REFDAC:
     * 0x03FF = power-down bit clear + full-scale code = 2.5 V output. */
    write_word(ADC168_W_PTR_REFDAC1);
    write_word(ADC168_W_REFDAC_2V5);
    write_word(ADC168_W_PTR_REFDAC2);
    write_word(ADC168_W_REFDAC_2V5);

    /* REFCM register (A=1100 then the value): route the internal reference
     * (REFIO1, now 2.5 V) as the pseudo-differential "negative input" for
     * all 8 channels. This gives every channel a 2.5 V +- 2.5 V input range
     * with NO jumper changes on the EVM. */
    write_word(ADC168_W_PTR_REFCM);
    write_word(ADC168_W_REFCM_INT);

    /* Reference settling: t_REFON = 8 ms max with the EVM's 22 uF REFIO
     * capacitors. Wait 10 ms of CPU cycles before trusting conversions. */
    __delay_cycles(MCLK_HZ / 100uL);

    /* Mode-change pipeline flush: SR/PDE/CID edits take effect "from the
     * next conversion with a delay of one read access" (§6.5.2.2). Burn two
     * complete conversion+read cycles so the streaming loop only ever sees
     * settled SR=1 framing — and so the channel pipeline is primed at
     * pair 0 for the first real scan. */
    for (i = 0; i < 2; i++) {
        (void)adc168_read_pair(0, &d0, &d1);
    }

    return status;
}

uint16_t adc168_link_readback(void)
{
    return s_link_readback;
}

/*
 * One complete conversion + readout cycle.
 *
 *  Timeline (8 MHz SCLK; total ~10 us of bus time per pair):
 *
 *    CONVST _|‾|__________________________________________________
 *    CLOCK  ____xxxxxxxxxxxxxxxxxxxxxxxx____xxxx...xxxx___________
 *                24 conversion clocks         40 readout clocks
 *    BUSY   ___|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|_________________________________
 *    RD     _______________________________|‾|_____________________
 *    SDOA   ------------------------------------[frame A][frame B]-
 *
 *  `next_pair` is PIPELINED: the command word shifted during THIS readout
 *  selects the mux input for the NEXT conversion. The caller compensates
 *  (see the scan loop in main.c); init pre-loads pair 0.
 */
adc168_result_t adc168_read_pair(uint8_t next_pair, int16_t *a, int16_t *b)
{
    /* Build the command byte: C[1:0] into bits 7:6 of the FIRST byte of the
     * 16-bit word (= word bits 15:14). R stays 00 = "update the channel
     * selection only, touch nothing else" — so a corrupted transfer can at
     * worst convert the wrong pair once, never wreck the configuration. */
    const uint8_t cword = (uint8_t)((next_pair & 0x03u) << 6);
    uint8_t rx[5];
    uint16_t ua, ub;
    uint16_t tries = ADC_BUSY_TIMEOUT;

    /* The datasheet forbids a CONVST rising edge while BUSY is high (it
     * would corrupt the running conversion). The flow guarantees BUSY is
     * long low by now, but a wiring fault could hold the pin high — better
     * a clean error than silent garbage. */
    if (ADC_BUSY()) {
        return ADC168_ERR_BUSY_TIMEOUT;
    }

    /* Freeze the sample-and-holds; conversion arms for the next CLOCK edge. */
    ADC_CONVST_PULSE();

    /* Feed the conversion: 3 bytes = 24 gated CLOCKs, comfortably above the
     * required ~17.5 conversion + 2 acquisition clocks. No RD pulse was
     * issued, so the ADC is not driving SDOA and ignores SDI — but we put
     * the command byte in the first slot anyway: if the part latched it
     * unexpectedly it would command the SAME next_pair (harmless). */
    (void)spi_xfer(cword);
    (void)spi_xfer(0x00);
    (void)spi_xfer(0x00);

    /* BUSY should already have fallen during the burst (after the ~18th
     * clock). Poll with a bounded loop so a dead ADC cannot hang the tick. */
    while (ADC_BUSY() && --tries) {
    }
    if (tries == 0u) {
        return ADC168_ERR_BUSY_TIMEOUT;
    }

    /* Readout: RD falling edge starts frame A; 40 clocks (5 bytes) stream
     * frame A then frame B on SDOA (SR=1). Simultaneously, the first 16
     * clocks latch our next-pair command word from SDI. */
    ADC_RD_PULSE();
    rx[0] = spi_xfer(cword);        /* also: command word byte 1 */
    rx[1] = spi_xfer(0x00);         /* also: command word byte 2 (0x00) */
    rx[2] = spi_xfer(0x00);
    rx[3] = spi_xfer(0x00);
    rx[4] = spi_xfer(0x00);

    /* Reassemble the two 16-bit results from the 40-bit stream.
     *
     *   bit#   39 38 | 37........22 | 21 20 | 19 18 | 17.........2 | 1 0
     *          0  0  | result A     | 0  0  | 0  1  | result B     | 0 0
     *          rx[0]..............rx[2]...........................rx[4]
     *
     * Every shift is done on uint16_t: on MSP430 `int` is 16 bits, so
     * shifting into bit 15 of a plain int would be undefined behavior. */
    ua = (uint16_t)(((uint16_t)(rx[0] & 0x3Fu) << 10) | /* A bits 15..10 */
                    ((uint16_t)rx[1] << 2) |            /* A bits  9..2  */
                    ((uint16_t)rx[2] >> 6));            /* A bits  1..0  */
    ub = (uint16_t)(((uint16_t)(rx[2] & 0x03u) << 14) | /* B bits 15..14 */
                    ((uint16_t)rx[3] << 6) |            /* B bits 13..6  */
                    ((uint16_t)rx[4] >> 2));            /* B bits  5..0  */
    *a = (int16_t)ua;               /* reinterpret as two's complement */
    *b = (int16_t)ub;

    /* Validate every fixed bit the framing guarantees:
     *   rx[0] bits 7:6   = 00   (leading zero + converter-A indicator 0)
     *   rx[2] bits 5:2   = 0001 (A's trailing 00, B's leading 0, B ind. 1)
     *   rx[4] bits 1:0   = 00   (B's trailing zeros)
     * Any mismatch means the bit alignment is off (wiring, clock phase,
     * strobe timing) and the numbers must not be trusted. */
    if ((rx[0] & 0xC0u) != 0x00u ||
        (rx[2] & 0x3Cu) != 0x04u ||
        (rx[4] & 0x03u) != 0x00u) {
        return ADC168_ERR_BAD_FRAME;
    }
    return ADC168_OK;
}
