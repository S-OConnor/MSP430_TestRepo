#ifndef ADC168M102_H
#define ADC168M102_H

#include <stdint.h>

/*
 * ADC168M102R-SEP driver — Mode II (M0=0 strap, M1=1) + special read (SR=1),
 * pseudo-differential (PDE=1), CID=0, half-clock, gated/burst CLOCK.
 * PDE=1 puts both input muxes in the pseudo-differential 4:1 configuration
 * (datasheet Table 6-2), where CONFIG C[1:0] selects one of CHx0..CHx3 per
 * converter and the negative input is that converter's common mode.
 * Pair k (k=0..3) converts CHAk and CHBk simultaneously against the internal
 * 2.5 V reference as common mode (REFCM). This firmware acquires ONE fixed
 * pair — ADC_PAIR in board.h, default 1 = CHA1 + CHB1 — so a tick is a single
 * conversion and both converters' results are kept.
 * All facts per datasheet SBASAW9 (docs/), register map Table 7-1/7-2.
 */

/* Control words (CONFIG bits: C[15:14] R[13:12] PD[11:10] FE9 SR8 FC7 PDE6
 * CID5 CE4 A[3:0]). ADC168_W_CONFIG_BASE has C=00; the driver ORs in
 * ADC_PAIR so the very first conversion already uses the wanted mux position. */
#define ADC168_W_RESET        0x0004u  /* A=0100: software reset               */
#define ADC168_W_CONFIG_BASE  0x1140u  /* R=01 full update, SR=1, PDE=1, CID=0 */
#define ADC168_W_LINKCHK      0x1041u  /* R=01, PDE=1, A=0001 read-CONFIG      */
#define ADC168_W_PTR_REFDAC1  0x1142u  /* CONFIG + A=x010: next word -> REFDAC1 */
#define ADC168_W_PTR_REFDAC2  0x1145u  /* CONFIG + A=x101: next word -> REFDAC2 */
#define ADC168_W_PTR_REFCM    0x114Cu  /* CONFIG + A=1100: next word -> REFCM   */
#define ADC168_W_REFDAC_2V5   0x03FFu  /* RPD=0 (enabled), code 0x3FF = 2.5 V  */
#define ADC168_W_REFCM_INT    0xFF00u  /* CMxx=1 (internal CM) x8, Rxx=0 = REFIO1 */

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

/* Convert pair ADC_PAIR and read both results: *a = CHA<ADC_PAIR>,
 * *b = CHB<ADC_PAIR>, sampled at the same instant. The channel-select command
 * is pipelined (it takes effect one conversion later), but since the pair
 * never changes, every access simply re-commands ADC_PAIR. */
adc168_result_t adc168_read(int16_t *a, int16_t *b);

#endif /* ADC168M102_H */
