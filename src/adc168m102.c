#include <stdbool.h>
#include <stddef.h>
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
 *  digitizes CHAk and CHBk together.
 *
 *  This firmware wants exactly two channels, and they are a pair: CHA1 and
 *  CHB1 (ADC_PAIR = 1 in board.h). That is the cheapest possible shape for
 *  this part — ONE conversion per tick, both converters' results kept, and
 *  the two samples taken at the same instant by construction. The mux
 *  selection is a constant, so there is no channel rotation to get wrong.
 *
 *  --- Mux configuration: the pseudo-differential 4:1 mode --------------------
 *
 *  PDE=1 selects "4x2 pseudo-differential operation" (Table 7-2), which puts
 *  each converter's front-end mux in the pseudo-differential 4:1 configuration
 *  of Table 6-2: four single-ended inputs per converter, each measured against
 *  that converter's common mode.
 *
 *      C[1:0]   ADC+     ADC-           (Table 6-2, restated in Table 7-2)
 *        00     CHx0     CMx / REFIOx
 *        01     CHx1     CMx / REFIOx   <- ADC_PAIR = 1
 *        10     CHx2     CMx / REFIOx
 *        11     CHx3     CMx / REFIOx
 *
 *  With PDE=0 the same two bits would instead pick one of TWO fully
 *  differential pairs (Table 6-1) — a different configuration we do not use.
 *
 *  Which source acts as "CMx / REFIOx" is the REFCM register's job: its CMxx
 *  bits pick the INTERNAL reference over the external CMA/CMB pins, and its
 *  Rxx bits pick REFIO1 (our 2.5 V DAC) over REFIO2.
 *
 *  Beware one misleading sentence in §6.3.2.1: "In pseudo-differential mode,
 *  channel selection is performed with the SEQFIFO register." That describes
 *  AUTOMATIC channel selection, i.e. M0 = 1 (Table 6-5). We strap M0 = 0
 *  (manual selection through SDI), and SEQFIFO keeps its reset value 0000h,
 *  whose SL[1:0] = 00 field reads "Do not use; use mode I or II instead, where
 *  M0 is 0". The sequencer is therefore inactive and C[1:0] above is what
 *  steers the mux. Never writing SEQFIFO also satisfies REFCM's "set this
 *  register after setting the SEQFIFO register" ordering note for free.
 *
 *  Unlike a normal SPI slave, the interface is built from five strobes/lines
 *  around the clock:
 *
 *    CLOCK   runs BOTH the SAR conversion and the data shifting. We supply it
 *            in gated bursts from the SPI peripheral (allowed per §6.3.1.4).
 *    CONVST  rising edge: sample-and-holds freeze; conversion starts on the
 *            next CLOCK rising edge and takes ~18 CLOCKs (half-clock mode).
 *            Held high across that first CLOCK, released on the second.
 *    BUSY    high while the conversion runs.
 *    RD      rising edge: the ADC starts driving result bits on SDOA; it
 *            also opens a 16-CLOCK window during which whatever appears on
 *            SDI is latched as a command/register word. Like CONVST it is
 *            held across the access's first CLOCK and released on the second
 *            (SBASAW9 Figure 5-1); see adc_access() below.
 *    ~CS     gate for SDI/RD/SDOA. Asserted for the duration of ONE access
 *            and released again between accesses, the way the PHI reference
 *            board drives it (docs/workingADC.jpg) — see adc_access() below.
 *
 *  Mode strapping (EVM header J5): M0 pin strapped to GND, M1 left pulled
 *  high -> "Mode II" = channel pair chosen manually by a command word, data
 *  on SDOA only. M1 high is also what deactivates SDOB (datasheet pin table:
 *  "Serial data output for converter B. Active only if M1 is low"), so SDOA
 *  is the single serial output for everything this part sends back.
 *
 *  This build runs PLAIN Mode II — SR ("special read") stays 0 (§6.5.2.2).
 *  One read access therefore delivers exactly ONE 20-bit frame on SDOA, and
 *  the two results of a conversion are fetched by two successive read
 *  accesses:
 *
 *      RD + 24 clocks  -> converter A's frame (CHA<ADC_PAIR>)
 *      RD + 24 clocks  -> converter B's frame (CHB<ADC_PAIR>)
 *
 *  (24 rather than 20 because the SPI moves whole bytes; the last 4 clocks
 *  are padding the ADC ignores.) The alternative, SR=1, packs both frames
 *  into a single 40-clock burst; it is not used here. The cost of plain Mode
 *  II is one extra RD strobe and 8 extra clocks per tick, and the gain is
 *  that every access on the bus has the same shape — RD plus three bytes —
 *  whether it is a register read or half a conversion result.
 *
 *  The converter each frame came from is not assumed: CID=0 keeps the A/B
 *  indicator bit in the frame, and adc168_read() checks it against the frame
 *  it expected. A part that returned the two frames in the other order (or
 *  the same frame twice) fails that check and is reported as
 *  ADC168_ERR_BAD_FRAME instead of silently swapping the two channels.
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
 *    C   next input, per the      R   write mode (00 = update C only,
 *        table above                     valid because M0=0; 01 = rewrite
 *    PD  power-down control              the whole CONFIG register)
 *    FE  FIFO enable             SR  special read (both frames per RD);
 *                                    0 here — see the readout note above
 *    FC  full-clock mode         PDE pseudo-differential enable
 *    CID channel-ID disable      CE  2-bit counter enable
 *    A   action/address: 0000 none, 0001 read CONFIG back, 0100 soft reset,
 *        x010/x101 "next word goes to REFDAC1/2", 1100 "... to REFCM", ...
 * =============================================================================
 */

static uint16_t s_link_readback;    /* raw CONFIG readback, published as g_cfg */

/* Operating CONFIG word: the fixed mode bits plus C = ADC_PAIR, so the first
 * conversion after configuration already selects the pair we stream
 * (pair 1 -> 0x5040). */
#define ADC168_W_CONFIG \
    ((uint16_t)(ADC168_W_CONFIG_BASE | ((uint16_t)(ADC_PAIR & 0x03) << 14)))

/* The same C field as it sits in the FIRST byte of a 16-bit command word
 * (word bits 15:14 = byte bits 7:6). */
#define ADC168_CWORD_BYTE   ((uint8_t)((ADC_PAIR & 0x03) << 6))

/*
 * ONE ACCESS — the single shape every exchange with this part takes:
 *
 *      ~CS    ‾‾‾|________________________|‾‾‾
 *      strobe ______|‾‾‾‾‾‾|_________________
 *      CLOCK  _________|‾|_|‾|_ ... _|‾|______
 *                      24 clocks, one contiguous burst
 *
 * ~CS is asserted for the access and released again as soon as the burst's
 * last clock edge has gone by, so the bus returns to fully idle — clock low,
 * ~CS high, both strobes low — between accesses. That is what the PHI
 * reference board does in docs/workingADC.jpg, where ~CS frames each burst
 * individually instead of sitting low across the whole session.
 *
 * Inside that frame sits the strobe (RD or CONVST), and it is NOT a pulse in
 * the gap before the burst. Per the datasheet's half-clock timing diagram
 * (SBASAW9 Figure 5-1) the strobe goes high ahead of the access — RD's rising
 * edge is what makes the ADC start driving SDOA (t3) and what opens the
 * 16-CLOCK SDI command window; CONVST's is what freezes the sample/holds — is
 * still high at the first rising CLOCK edge, and comes back down within one
 * CLOCK period of it (t2/t3).
 *
 * The split of work follows from that:
 *
 *   - spi_wait_ready() BEFORE ~CS falls, so the bus is provably idle and
 *     nothing can block between ~CS, the strobe and the first clock edge.
 *     Doing the waiting the other way round — assert, then poll the SPI —
 *     would put an open-ended wait inside the strobe's high time, which the
 *     datasheet caps at one CLOCK period.
 *   - both strobe edges inside spi_burst_strobe(), which is the only code
 *     that can place them against the clock edges it starts.
 *   - spi_wait_ready() again before ~CS rises. spi_burst_strobe() returns
 *     once the last byte has been received, which is the burst's final clock
 *     edge, but UCBUSY can still be set for a few cycles afterwards; polling
 *     it keeps the ~CS rising edge strictly after the last CLOCK edge, the
 *     order the diagram draws. It costs nothing — the condition is already
 *     true almost every time.
 *
 * ~CS gates SDI, RD and SDOA (§6.3.1, "Bring CS low to enable both serial
 * outputs"); the only timing the datasheet puts on it is tD6 = 6 ns from the
 * ~CS rising edge to SDOA tri-stating, which is far shorter than the single
 * instruction between the edges here.
 */
ALWAYS_INLINE void adc_access(const uint8_t *tx, uint8_t *rx,
                              volatile uint8_t *port, uint8_t mask)
{
    spi_wait_ready();
    ADC_CS_LOW();

    spi_burst_strobe(tx, rx, 3u, port, mask);

    spi_wait_ready();
    ADC_CS_HIGH();
}

/* The RD-opened flavour: register writes, register reads and conversion
 * readouts all go through here. */
ALWAYS_INLINE void rd_access(const uint8_t *tx, uint8_t *rx)
{
    adc_access(tx, rx, ADC_RD_PORT, ADC_RD_BIT);
}

/*
 * Write one 16-bit word into the ADC.
 *
 * RD opens the SDI latch window; the next 16 CLOCK falling edges (= our first
 * two SPI bytes) clock the word in, MSB first.
 *
 * The third, dummy byte matters: the datasheet says a register update only
 * becomes ACTIVE "with the CLOCK rising edge after completing the 16-clock
 * write access" (§7). With a free-running clock that edge arrives naturally;
 * with our gated clock it would never come until the next access — so we
 * append 8 extra clocks to deliver it immediately. All three bytes go out in
 * one burst so those 24 clocks are contiguous — the activation edge has to be
 * part of the same access, not a separate burst after a gap.
 *
 * Deliberately NOT ALWAYS_INLINE, unlike the other wrappers here: it has eight
 * call sites in adc168_init() alone, and inlining it (with adc_access folded
 * in) there costs several hundred bytes of FRAM. It buys no stack back either
 * — a register write is main -> write_word -> spi_burst_strobe, three levels
 * shallower than the readout chain that sets the high-water mark.
 */
static void write_word(uint16_t w)
{
    const uint8_t tx[3] = { (uint8_t)(w >> 8),      /* bits 15..8 */
                            (uint8_t)w,             /* bits  7..0 */
                            0x00u };                /* activation clocks */

    rd_access(tx, NULL);
}

/*
 * One READ ACCESS — the same RD-opened, 3-byte shape as a write (§6.5.2.2,
 * now that SR is 0), read from the other direction.
 *
 * RD makes the ADC start driving SDOA; the frame that comes back is 20 bits
 * and the last 4 clocks are padding the part ignores:
 *
 *   b[0] = [lead 0][A/B ind][d15..d10]
 *   b[1] = [d9 .. d2]
 *   b[2] = [d1][d0][0][0][x x x x]
 *
 * For a register read (A=0001 etc.) the middle 16 bits are the register
 * value; for a conversion readout they are one converter's result. The same
 * RD assertion also opens the 16-CLOCK SDI window, so `cmd` rides out in the
 * first byte — callers pass the channel command there, or 0x00 when there is
 * nothing to say.
 */
ALWAYS_INLINE void read_access(uint8_t cmd, uint8_t *b)
{
    const uint8_t tx[3] = { cmd, 0x00u, 0x00u };

    rd_access(tx, b);
}

/* The 16 payload bits of a read access, with the leading zero, the A/B
 * indicator and the trailing zeros stripped off. */
ALWAYS_INLINE uint16_t frame_data(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)(b[0] & 0x3Fu) << 10) |    /* d15..d10 */
                      ((uint16_t)b[1] << 2) |               /* d9..d2   */
                      ((uint16_t)b[2] >> 6));               /* d1..d0   */
}

/*
 * Read one 16-bit register back (used by adc168_config_cycle() only).
 *
 * After a "read register" command (A=0001 etc.), the ADC presents the value
 * during the NEXT access. The two leading bits are indicator bits rather than
 * data, and frame_data() already drops them.
 */
ALWAYS_INLINE uint16_t read_word(void)
{
    uint8_t b[3];

    read_access(0x00, b);
    return frame_data(b);
}

/*
 * One CONFIG write + read-back exchange — the "is the link still there?"
 * probe, used both once at init and repeatedly (once a second) by the idle
 * phase while the firmware waits for a button press.
 *
 * It writes ADC168_W_LINKCHK: the mode bits we care about plus A=0001,
 * "present CONFIG on SDOA at the next read access", then fetches the reply.
 * That word carries C=00, so it parks the mux on pair 0 and leaves the part
 * NOT selecting the pair we stream — which is exactly why
 * adc168_start_stream() below must run before the first real sample.
 *
 * On the scope this is a compact, unmistakable signature: two RD pulses with
 * 24 clocks each and ~1 s of dead bus either side.
 */
uint16_t adc168_config_cycle(void)
{
    write_word(ADC168_W_LINKCHK);
    s_link_readback = read_word();
    return s_link_readback;
}

/*
 * Does a CONFIG read-back look like the word we just wrote?
 *
 * Bits 11:4 carry PD=00, FE=0, SR=0, FC=0, PDE=1, CID=0, CE=0 -> 0x04. A
 * dead SDOA line (all 0s or all 1s) fails this, as does a clock-phase or
 * strobe fault that slides the frame by a bit.
 */
bool adc168_config_ok(uint16_t raw)
{
    return (((raw >> 4) & 0xFFu) == 0x04u);
}

/*
 * Leave the config-probe state and arm continuous acquisition.
 *
 * Writes the real operating CONFIG (R=01 rewrite, SR=0 plain Mode II, PDE=1,
 * C=ADC_PAIR), then burns two complete conversion+read cycles because
 * SR/PDE/CID edits only take effect "from the next conversion with a delay of
 * one read access" (§6.5.2.2) — at init PDE really does change (the software
 * reset cleared it), so without the flush the first samples would be read
 * against the old mux configuration.
 *
 * Called at the end of init and again every time the idle phase hands over to
 * streaming. Not optional in either case: the probe word carries C=00, so
 * without this rewrite the first conversions would digitize pair 0 instead of
 * ADC_PAIR — the right framing, but the wrong channels.
 */
void adc168_start_stream(void)
{
    int16_t d0, d1;
    uint8_t i;

    write_word(ADC168_W_CONFIG);

    for (i = 0; i < 2; i++) {
        (void)adc168_read(&d0, &d1);
    }
}

uint8_t adc168_init(void)
{
    uint8_t status = 0;

    /* ~CS is NOT parked low here. Every access asserts and releases it for
     * itself (see adc_access() above), so the line already idles high from
     * clocks.c and each exchange below frames itself. */

    /* Software reset (A=0100): all registers to power-up defaults, any
     * ongoing conversion aborted. Interface is usable again ~20 ns later —
     * far shorter than the gap to our next SPI byte. */
    write_word(ADC168_W_RESET);

    /* --- Link check ------------------------------------------------------
     * Prove the SPI wiring/phase and the RD strobe work before touching the
     * real configuration: write CONFIG with PDE=1 plus the "read CONFIG
     * back" action (A=0001), then fetch the reply and compare the mode bits
     * we just wrote (bits 11:4 must read PD=00,FE=0,SR=0,FC=0,PDE=1,CID=0,
     * CE=0 = 0x04) — the same mode bits the operating word uses, so this
     * probe exercises exactly the framing streaming will run in. A dead MISO
     * line (0x0000/0xFFFF) fails this. */
    if (!adc168_config_ok(adc168_config_cycle())) {
        status |= ST_ADC_NOLINK;
    }

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
     * (REFIO1, now 2.5 V) as the pseudo-differential "negative input" — the
     * "CMx / REFIOx" column of Table 6-2. CMxx=1 picks the internal source
     * over the external CMA/CMB pins; Rxx=0 picks REFIO1 over REFIO2. Only
     * the ADC_PAIR channels are ever read, but arming all 8 bits costs
     * nothing and keeps ADC_PAIR a one-line change. Every channel then has a
     * 2.5 V +- 2.5 V input range with NO jumper changes on the EVM. */
    write_word(ADC168_W_PTR_REFCM);
    write_word(ADC168_W_REFCM_INT);

    /* Reference settling: t_REFON = 8 ms max with the EVM's 22 uF REFIO
     * capacitors. Wait 10 ms of CPU cycles before trusting conversions. */
    __delay_cycles(MCLK_HZ / 100uL);

    /* Write the real operating CONFIG and flush the mode-change pipeline.
     * main() calls this again on the button press; init just leaves the part
     * in the same armed state, so a build that skipped the idle phase behaves
     * identically to one that sat in it for an hour. */
    adc168_start_stream();

    return status;
}

uint16_t adc168_link_readback(void)
{
    return s_link_readback;
}

/*
 * One complete conversion + readout cycle — the entire per-tick ADC workload.
 *
 *  Timeline (0.5 MHz SCLK — 2 us per clock; total ~150 us of bus time):
 *
 *    ~CS    ‾|_____________|‾‾‾|____________|‾‾‾|____________|‾‾‾‾‾‾
 *    CONVST _|‾‾‾|___________________________________________________
 *    CLOCK  ___xxxxxxxxxxxxx____xxxxxxxxxxxx____xxxxxxxxxxxx_________
 *              24 conversion     24 readout      24 readout
 *                  clocks        clocks (A)      clocks (B)
 *    BUSY   ___|‾‾‾‾‾‾‾‾|________________________________________
 *    RD     __________________|‾‾‾|_________|‾‾‾|_________________
 *    SDOA   --------------------[ frame A ]-----[ frame B ]--------
 *
 *  Each strobe rises just before its burst and falls on the burst's SECOND
 *  rising CLOCK edge — it straddles the first clock rather than sitting in
 *  the gap before it (SBASAW9 Figure 5-1; spi_burst_strobe() in spi.c).
 *
 *  ~CS frames each of the three bursts on its own and goes back high in
 *  between, so a tick shows three self-contained accesses on the scope
 *  rather than one long selected window — the shape of docs/workingADC.jpg.
 *
 *  Three bursts of three bytes each: one to clock the conversion, then one
 *  read access per converter. With SR=0 a read access yields exactly one
 *  frame (§6.5.2.2), so converter B needs its own RD pulse — that second
 *  strobe is the whole difference from the SR=1 arrangement.
 *
 *  The channel-select command is PIPELINED: the C field shifted during a
 *  readout selects the mux input for the NEXT conversion. We always send
 *  ADC_PAIR, so the selection is self-sustaining — every access re-asserts
 *  the same pair, and init already primed it through the CONFIG word.
 */
adc168_result_t adc168_read(int16_t *a, int16_t *b)
{
    /* Command byte: C[1:0] in bits 7:6 of the FIRST byte of the 16-bit word
     * (= word bits 15:14). R stays 00 = "update the channel selection only,
     * touch nothing else" — so a corrupted transfer can at worst convert the
     * wrong pair once, and the next access puts it straight back. */
    const uint8_t cword = ADC168_CWORD_BYTE;
    const uint8_t conv_tx[3] = { ADC168_CWORD_BYTE, 0x00u, 0x00u };
    uint8_t fa[3], fb[3];
    uint16_t tries = ADC_BUSY_TIMEOUT;

    /* The datasheet forbids a CONVST rising edge while BUSY is high (it
     * would corrupt the running conversion). The flow guarantees BUSY is
     * long low by now, but a wiring fault could hold the pin high — better
     * a clean error than silent garbage. */
    if (ADC_BUSY()) {
        return ADC168_ERR_BUSY_TIMEOUT;
    }

    /* Freeze the sample-and-holds and clock the conversion in one go.
     * The adc_access() wrapper waits for an idle bus before anything
     * moves, for the same reason as on a read access: the conversion starts
     * on the first CLOCK rising edge after CONVST, so every cycle between the
     * two is time the sample sits in hold, drooping, before the SAR gets to
     * it. Then 3 bytes = 24 gated CLOCKs in one contiguous burst, comfortably
     * above the required ~17.5 conversion + 2 acquisition clocks. CONVST is
     * asserted just ahead of the burst (spi_burst_strobe() raises it), is
     * still high at the first rising CLOCK edge — the edge the conversion
     * starts on — and is released by spi_burst_strobe() on the second, one
     * CLOCK period later, which is the width the datasheet draws (SBASAW9
     * Figure 5-1). ~CS goes low with it and comes back up once the burst
     * ends, exactly as on a read access. No RD was issued, so the ADC is not
     * driving SDOA and
     * ignores SDI — but we put the command byte in the first slot anyway: if
     * the part latched it unexpectedly it would command the SAME pair
     * (harmless). */
    adc_access(conv_tx, NULL, ADC_CONVST_PORT, ADC_CONVST_BIT);

    /* BUSY should already have fallen during the burst: the conversion ends
     * after the ~18th clock (36 us at 0.5 MHz) and the burst runs 24 clocks
     * (48 us). Poll with a bounded loop anyway so a dead ADC cannot hang the
     * tick. */
    while (ADC_BUSY() && --tries) {
    }
    if (tries == 0u) {
        return ADC168_ERR_BUSY_TIMEOUT;
    }

    /* Readout, two read accesses: the first RD rising edge starts converter
     * A's frame, the second starts converter B's. Each also opens a 16-CLOCK
     * SDI window, so the pair command rides out in both — re-asserting the
     * same selection twice is harmless (R=00 touches nothing else). */
    read_access(cword, fa);
    read_access(cword, fb);

    *a = (int16_t)frame_data(fa);   /* reinterpret as two's complement */
    *b = (int16_t)frame_data(fb);

    /* Validate every fixed bit the framing guarantees, in both frames:
     *   b[0] bit 7    = 0   (constant leading zero)
     *   b[0] bit 6    = A/B converter indicator — 0 in the first frame,
     *                   1 in the second. Checking it is what makes the
     *                   "A then B" ordering an assertion rather than an
     *                   assumption: a part that answered in the other order,
     *                   or repeated one frame, fails here instead of
     *                   silently swapping the two channels.
     *   b[2] bits 5:4 = 00  (the frame's trailing zeros; bits 3:0 are the
     *                        4 padding clocks and carry nothing)
     * Any mismatch means the bit alignment is off (wiring, clock phase,
     * strobe timing) and the numbers must not be trusted. */
    if ((fa[0] & 0xC0u) != 0x00u || (fa[2] & 0x30u) != 0x00u ||
        (fb[0] & 0xC0u) != 0x40u || (fb[2] & 0x30u) != 0x00u) {
        return ADC168_ERR_BAD_FRAME;
    }
    return ADC168_OK;
}
