/* libFLAC - Free Lossless Audio Codec library
 *
 * Xtensa assembly wrappers for FLAC LPC restoration.
 *
 * Bridges libFLAC's FLAC__lpc_restore_signal* API to the optimized
 * Xtensa assembly routines adapted from micro-flac (Kevin Ahrendt).
 *
 * Key transformations performed here:
 *
 *   1. Coefficient reversal
 *        libFLAC   : data[i-1] * qlp_coeff[0], data[i-2] * qlp_coeff[1], ...
 *        micro-flac: buffer[0] * coef[0],     buffer[1] * coef[1],     ...
 *      -> qlp_coeff[] is reversed while copying into coeff16[].
 *
 *   2. Coefficient narrowing
 *        FLAC qlp_coeff is int32 but effective precision <= 15 bits
 *        (FLAC__MAX_QLP_COEFF_PRECISION), so int16 is lossless.
 *
 *   3. Buffer layout unification
 *        micro-flac keeps warm-up at buffer[0..order-1] and residuals at
 *        buffer[order..] in a single buffer, restoring in place.
 *        libFLAC splits these into data[-order..-1] and residual[0..N-1].
 *        We copy residual[] into data[] and pass (data - order) as the
 *        unified buffer.  In-place restore is safe because iteration k
 *        reads buffer[k..k+order-1] (= data[k-order..k-1], already
 *        restored or warm-up) and writes buffer[k+order] (= data[k],
 *        not yet touched).
 *
 *   4. No heap allocation
 *        Coefficient staging uses a stack-resident int16_t[12].
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "./xtensa/lpc_xtensa.h"

#if (FLAC_LPC_XTENSA_ENABLED == 1)

#include <string.h>
#include <stdint.h>

#include "FLAC/assert.h"
#include "FLAC/format.h"     /* FLAC__int32, FLAC__MAX_LPC_ORDER */

/* Sanity: the assembly handlers only cover orders 1..12. */
#if FLAC__MAX_LPC_ORDER < 12
#  error "FLAC__MAX_LPC_ORDER smaller than 12; adjust the wrapper bounds"
#endif

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Reverse and narrow the qlp_coeff[] array into a stack int16_t buffer.
 *
 * Preconditions (checked by callers):
 *   - 1 <= order <= 12
 *   - lp_quantization in [0, 31]
 */
static inline void
prepare_coefficients(const FLAC__int32 *qlp_coeff, uint32_t order,
                     int16_t *out)
{
    uint32_t i;
    FLAC__ASSERT(order >= 1);
    FLAC__ASSERT(order <= 12);

    for (i = 0; i < order; i++) {
        out[i] = (int16_t)qlp_coeff[order - 1 - i];
    }
}

/*
 * Copy residual[] into data[] unless they are the same buffer.
 * Both are guaranteed by libFLAC to have room for data_len elements.
 */
static inline void
stage_residuals(const FLAC__int32 *residual,
                uint32_t data_len,
                FLAC__int32 *data)
{
    if (residual != (const FLAC__int32 *)data) {
        memmove(data, residual, (size_t)data_len * sizeof(FLAC__int32));
    }
}

/* ------------------------------------------------------------------------- */
/* 32-bit accumulator wrapper                                                */
/* ------------------------------------------------------------------------- */

/*
 * Called from FLAC__lpc_restore_signal().
 *
 * The 32-bit path relies on the FLAC format guarantee that the final
 * prediction sum fits in 32 bits.  MULL (low 32 bits only) therefore
 * matches the C code's two's-complement 32-bit arithmetic exactly.
 *
 * Returns non-zero if the assembly path handled the call, zero to
 * indicate the caller should fall back to the C implementation.
 */
int
flac_lpc_restore_signal_esp32s3(const FLAC__int32 *residual,
                                uint32_t data_len,
                                const FLAC__int32 *qlp_coeff,
                                uint32_t order,
                                int lp_quantization,
                                FLAC__int32 *data)
{
    int16_t coeff16[12];
    int32_t *buffer;

    /* Bounds the assembly handlers cannot express. */
    if (order == 0 || order > 12) {
        return 0;
    }
    if (lp_quantization < 0 || lp_quantization > 31) {
        return 0;
    }
    if (data_len == 0) {
        /* Nothing to restore; treat as handled. */
        return 1;
    }

    prepare_coefficients(qlp_coeff, order, coeff16);
    stage_residuals(residual, data_len, data);

    /*
     * micro-flac buffer convention:
     *   buffer[0..order-1]  = warm-up samples
     *   buffer[order..]     = residuals, restored in place
     *
     * libFLAC guarantees data[-order..-1] holds the warm-up samples,
     * so (data - order) lines up exactly.
     */
    buffer = (int32_t *)data - order;

    restore_lpc_32bit_asm(buffer,
                          (size_t)order + (size_t)data_len,
                          coeff16,
                          order,
                          (int32_t)lp_quantization);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* 64-bit accumulator wrapper                                                */
/* ------------------------------------------------------------------------- */

/*
 * Called from FLAC__lpc_restore_signal_wide().
 *
 * Uses MULL/MULSH to build full 64-bit products and propagates carries
 * into a 64-bit accumulator, matching libFLAC's "wide" path exactly
 * for 24-bit and 33-bit material.
 *
 * Returns non-zero if the assembly path handled the call.
 */
int
flac_lpc_restore_signal_wide_esp32s3(const FLAC__int32 *residual,
                                     uint32_t data_len,
                                     const FLAC__int32 *qlp_coeff,
                                     uint32_t order,
                                     int lp_quantization,
                                     FLAC__int32 *data)
{
    int16_t coeff16[12];
    int32_t *buffer;

    if (order == 0 || order > 12) {
        return 0;
    }
    if (lp_quantization < 0 || lp_quantization > 31) {
        return 0;
    }
    if (data_len == 0) {
        return 1;
    }

    prepare_coefficients(qlp_coeff, order, coeff16);
    stage_residuals(residual, data_len, data);

    buffer = (int32_t *)data - order;

    restore_lpc_64bit_asm(buffer,
                          (size_t)order + (size_t)data_len,
                          coeff16,
                          order,
                          (int32_t)lp_quantization);
    return 1;
}

#endif /* FLAC_LPC_XTENSA_ENABLED */