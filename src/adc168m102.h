#ifndef ADC168M102_H
#define ADC168M102_H

#include <stdint.h>
#include <stdbool.h>

/*
 * ADC168M102R-SEP driver — plain Mode II (M0=0 strap, M1=1), special read
 * DISABLED (SR=0), pseudo-differential (PDE=1), CID=0, half-clock,
 * gated/burst CLOCK.
 * PDE=1 puts both input muxes in the pseudo-differential 4:1 configuration
 * (datasheet Table 6-2), where CONFIG C[1:0] selects one of CHx0..CHx3 per
 * converter and the negative input is that converter's common mode.
 * Pair k (k=0..3) converts CHAk and CHBk simultaneously against the internal
 * 2.5 V reference as common mode (REFCM). This firmware acquires ONE fixed
 * pair — ADC_PAIR in board.h, default 1 = CHA1 + CHB1 — so a tick is a single
 * conversion and both converters' results are kept.
 *
 * Readout shape (ADC §6.5.2.2, plain Mode II): SDOA is the only data output —
 * M1 is pulled high on the EVM, which leaves SDOB inactive — and ONE read
 * access delivers ONE 20-bit frame. So the two results of a conversion are
 * fetched by TWO successive read accesses: RD + 24 clocks for converter A,
 * then RD + 24 clocks for converter B. Each frame carries a converter
 * indicator bit (CID=0), and the driver checks it against the frame it
 * expected — so if the part ever delivered the frames in another order, that
 * shows up as ADC168_ERR_BAD_FRAME rather than as silently swapped channels.
 * All facts per datasheet SBASAW9 (docs/), register map Table 7-1/7-2.
 */

/* Control words (CONFIG bits: C[15:14] R[13:12] PD[11:10] FE9 SR8 FC7 PDE6
 * CID5 CE4 A[3:0]). ADC168_W_CONFIG_BASE has C=00; the driver ORs in
 * ADC_PAIR so the very first conversion already uses the wanted mux position.
 * Each constant is prefaced by its 16-bit value in binary, MSB first, split
 * into its register fields with a name ruler above it. The last two words are
 * not CONFIG writes and use their own layouts (ADC Table 7-1): REFDACx is
 * Reserved[15:11] RPD D[9:0], REFCM is CMB[15:12] CMA[11:8] RB[7:4] RA[3:0]. */

// Software reset — the first word adc168_init() sends once ~CS is low.
// A=0100 returns every register to its power-up default and aborts any
// conversion in flight, so bring-up starts from a known state whatever ran
// before (a warm restart, a debugger halt mid-stream).
// C  R  PD FE SR FC PDE CID CE A
// 00 00 00 0  0  0  0   0   0  0100
#define ADC168_W_RESET        0x0004u

// The operating configuration, minus the channel. adc168m102.c ORs ADC_PAIR
// into C[15:14] to build ADC168_W_CONFIG (0x5040 for pair 1) and writes that
// once at init; the mode bits never change afterwards. R=01 rewrites the
// whole register, SR=0 keeps plain Mode II framing — one read access, one
// 20-bit frame on SDOA, so a conversion's two results take two read accesses
// (ADC §6.5.2.2) — PDE=1 selects the pseudo-differential 4:1 mux, and CID=0
// keeps the converter indicator bit the frame validator checks. C=00 here is
// only a placeholder — this constant is never written as-is.
// C  R  PD FE SR FC PDE CID CE A
// 00 01 00 0  0  0  1   0   0  0000
#define ADC168_W_CONFIG_BASE  0x1040u

// Link check, written before the real configuration so a wiring or clock-
// phase fault is caught before it can be mistaken for bad data. This is
// exactly ADC168_W_CONFIG_BASE with A=0001, which tells the ADC to present
// CONFIG on SDOA at the next read access; init compares bits 11:4 of the
// reply against the PDE=1 it just wrote and sets ST_ADC_NOLINK on a mismatch,
// which is what a stuck SDOA (all 0s or all 1s) produces. Only the C field
// differs from the operating word — the mode bits are identical — so the
// probe leaves framing alone and merely parks the mux on pair 0.
// C  R  PD FE SR FC PDE CID CE A
// 00 01 00 0  0  0  1   0   0  0001
#define ADC168_W_LINKCHK      0x1041u

// Address word for REFDAC1. Registers other than CONFIG have no address
// byte: you write CONFIG carrying an action code and the FOLLOWING word
// lands in the target register. A=x010 aims the next write at REFDAC1, whose
// payload is ADC168_W_REFDAC_2V5. The mode bits match ADC168_W_CONFIG_BASE
// so steering a write cannot disturb the configuration.
// C  R  PD FE SR FC PDE CID CE A
// 00 01 00 0  0  0  1   0   0  0010
#define ADC168_W_PTR_REFDAC1  0x1042u

// Address word for REFDAC2 — same two-step pattern as ADC168_W_PTR_REFDAC1,
// with A=x101 selecting the second reference DAC.
// C  R  PD FE SR FC PDE CID CE A
// 00 01 00 0  0  0  1   0   0  0101
#define ADC168_W_PTR_REFDAC2  0x1045u

// Address word for REFCM (A=1100), steering the next write to the common-
// mode routing register. Payload is ADC168_W_REFCM_INT.
// C  R  PD FE SR FC PDE CID CE A
// 00 01 00 0  0  0  1   0   0  1100
#define ADC168_W_PTR_REFCM    0x104Cu

// Payload written into both reference DACs. They come out of reset DISABLED
// (REFDACx default 0x07FF has the power-down bit set), so this word does two
// jobs: RPD=0 powers the DAC up, and full-scale code 0x3FF sets its output
// to 2.5 V on REFIO1/REFIO2. Sent twice, once after each PTR_REFDACx word,
// then given t_REFON = 8 ms to settle into the EVM's 22 uF reference caps.
// Reserved RPD D
// 00000    0   1111111111
#define ADC168_W_REFDAC_2V5   0x03FFu

// Payload for REFCM: routes the now-2.5 V internal reference as the pseudo-
// differential negative input for all eight channels. CMxx=1 picks the
// internal common mode over the external CMA/CMB pins; Rxx=0 picks REFIO1
// over REFIO2. Only the ADC_PAIR channels are ever read, but arming all
// eight bits costs nothing and keeps ADC_PAIR a one-line change. Every
// channel then spans 2.5 V +- 2.5 V with no EVM jumper changes.
// CMB  CMA  RB   RA
// 1111 1111 0000 0000
#define ADC168_W_REFCM_INT    0xFF00u

typedef enum {
    ADC168_OK = 0,
    ADC168_ERR_BUSY_TIMEOUT,    /* BUSY never fell after conversion clocks */
    ADC168_ERR_BAD_FRAME        /* indicator/zero bits failed validation   */
} adc168_result_t;

/* Reset, verify the register link (CONFIG readback), program mode + internal
 * references + common-mode routing, wait t_REFON, flush the mode-change
 * pipeline with two discarded conversions. Returns 0 or ST_ADC_NOLINK.
 * The raw CONFIG readback is kept for the caller to publish (g_cfg). */
uint8_t adc168_init(void);
uint16_t adc168_link_readback(void);

/* Write CONFIG with the "read CONFIG back" action and fetch the reply, i.e.
 * one write+read exchange with the register. This is what main() repeats once
 * a second in the idle phase while it waits for a LaunchPad button; the
 * returned word is the raw readback (publish it, or hand it to
 * adc168_config_ok()). The probe word carries C=00, so it parks the mux on
 * pair 0 — acquisition is NOT armed afterwards and adc168_start_stream() must
 * run before the first sample. */
uint16_t adc168_config_cycle(void);

/* True if a CONFIG readback matches what was written (mode bits 11:4 = 0x04).
 * Same test adc168_init() uses to decide ST_ADC_NOLINK. */
bool adc168_config_ok(uint16_t raw);

/* Arm continuous acquisition: write the operating CONFIG (SR=0, PDE=1,
 * C=ADC_PAIR) and burn two conversions to flush the mode-change pipeline.
 * Called by adc168_init(), and again by main() when a button ends the idle
 * phase — the idle probe leaves C=00 behind, so this is mandatory before
 * adc168_read() results can be trusted. */
void adc168_start_stream(void);

/* Convert pair ADC_PAIR and read both results: *a = CHA<ADC_PAIR>,
 * *b = CHB<ADC_PAIR>, sampled at the same instant by one conversion, then
 * fetched by two successive read accesses (converter A's frame, then
 * converter B's). The channel-select command is pipelined (it takes effect one
 * conversion later), but since the pair never changes, every access simply
 * re-commands ADC_PAIR. */
adc168_result_t adc168_read(int16_t *a, int16_t *b);

#endif /* ADC168M102_H */
