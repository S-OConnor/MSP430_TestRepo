#ifndef ADC168M102_H
#define ADC168M102_H

#include <stdint.h>

/*
 * ADC168M102R-SEP driver — Mode II (M0=0 strap, M1=1) + special read (SR=1),
 * pseudo-differential (PDE=1), CID=0, half-clock, gated/burst CLOCK.
 * Pair k (k=0..3) converts CHAk and CHBk simultaneously against the internal
 * 2.5 V reference as common mode (REFCM); 4 pairs cover all 8 channels.
 * All facts per datasheet SBASAW9 (docs/), register map Table 7-1/7-2.
 */

/* Control words (CONFIG bits: C[15:14] R[13:12] PD[11:10] FE9 SR8 FC7 PDE6
 * CID5 CE4 A[3:0]). */
#define ADC168_W_RESET        0x0004u  /* A=0100: software reset               */
#define ADC168_W_CONFIG       0x1140u  /* R=01 full update, SR=1, PDE=1, CID=0 */
#define ADC168_W_LINKCHK      0x1041u  /* R=01, PDE=1, A=0001 read-CONFIG      */
#define ADC168_W_PTR_REFDAC1  0x1142u  /* CONFIG + A=x010: next word -> REFDAC1 */
#define ADC168_W_PTR_REFDAC2  0x1145u  /* CONFIG + A=x101: next word -> REFDAC2 */
#define ADC168_W_PTR_REFCM    0x114Cu  /* CONFIG + A=1100: next word -> REFCM   */
#define ADC168_W_REFDAC_2V5   0x03FFu  /* RPD=0 (enabled), code 0x3FF = 2.5 V  */
#define ADC168_W_REFCM_INT    0xFF00u  /* all 8 channels: internal REFIO1 CM   */

typedef enum {
    ADC168_OK = 0,
    ADC168_ERR_BUSY_TIMEOUT,    /* BUSY never fell after conversion clocks */
    ADC168_ERR_BAD_FRAME        /* indicator/zero bits failed validation   */
} adc168_result_t;

/* Reset, verify the register link (CONFIG readback), program mode + internal
 * references + common-mode routing, wait t_REFON, flush the mode-change
 * pipeline with two discarded conversions. Returns 0 or ST_ADC_NOLINK.
 * Raw CONFIG readback is stored for the boot banner. */
uint8_t adc168_init(void);
uint16_t adc168_link_readback(void);

/* Convert the pair selected by the previous access, read both results, and
 * queue `next_pair` (0..3) for the following conversion (C word is pipelined:
 * it takes effect one conversion later). */
adc168_result_t adc168_read_pair(uint8_t next_pair, int16_t *a, int16_t *b);

#endif /* ADC168M102_H */
