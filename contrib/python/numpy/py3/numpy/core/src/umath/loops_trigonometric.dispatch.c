#line 1 "numpy/core/src/umath/loops_trigonometric.dispatch.c.src"

/*
 *****************************************************************************
 **       This file was autogenerated from a template  DO NOT EDIT!!!!      **
 **       Changes should be made to the original source (.src) file         **
 *****************************************************************************
 */

#line 1
/*@targets
 ** $maxopt baseline
 ** (avx2 fma3) avx512f
 ** vsx2 vsx3 vsx4
 ** neon_vfpv4
 **/
#include "numpy/npy_math.h"
#include "contrib/python/numpy/py3/numpy/core/src/common/simd/simd.h"
#include "loops_utils.h"
#include "loops.h"
/*
 * TODO:
 * - use vectorized version of Payne-Hanek style reduction for large elements or
 *   when there's no native FUSED support instead of fallback to libc
 */
#if NPY_SIMD_FMA3 // native support
/*
 * Vectorized Cody-Waite range reduction technique
 * Performs the reduction step x* = x - y*C in three steps:
 * 1) x* = x - y*c1
 * 2) x* = x - y*c2
 * 3) x* = x - y*c3
 * c1, c2 are exact floating points, c3 = C - c1 - c2 simulates higher precision
 */
NPY_FINLINE npyv_f32
simd_range_reduction_f32(npyv_f32 x, npyv_f32 y, npyv_f32 c1, npyv_f32 c2, npyv_f32 c3)
{
    npyv_f32 reduced_x = npyv_muladd_f32(y, c1, x);
    reduced_x = npyv_muladd_f32(y, c2, reduced_x);
    reduced_x = npyv_muladd_f32(y, c3, reduced_x);
    return reduced_x;
}
/*
 * Approximate cosine algorithm for x \in [-PI/4, PI/4]
 * Maximum ULP across all 32-bit floats = 0.875
 */
NPY_FINLINE npyv_f32
simd_cosine_poly_f32(npyv_f32 x2)
{
    const npyv_f32 invf8 = npyv_setall_f32(0x1.98e616p-16f);
    const npyv_f32 invf6 = npyv_setall_f32(-0x1.6c06dcp-10f);
    const npyv_f32 invf4 = npyv_setall_f32(0x1.55553cp-05f);
    const npyv_f32 invf2 = npyv_setall_f32(-0x1.000000p-01f);
    const npyv_f32 invf0 = npyv_setall_f32(0x1.000000p+00f);

    npyv_f32 r = npyv_muladd_f32(invf8, x2, invf6);
    r = npyv_muladd_f32(r, x2, invf4);
    r = npyv_muladd_f32(r, x2, invf2);
    r = npyv_muladd_f32(r, x2, invf0);
    return r;
}
/*
 * Approximate sine algorithm for x \in [-PI/4, PI/4]
 * Maximum ULP across all 32-bit floats = 0.647
 * Polynomial approximation based on unpublished work by T. Myklebust
 */
NPY_FINLINE npyv_f32
simd_sine_poly_f32(npyv_f32 x, npyv_f32 x2)
{
    const npyv_f32 invf9 = npyv_setall_f32(0x1.7d3bbcp-19f);
    const npyv_f32 invf7 = npyv_setall_f32(-0x1.a06bbap-13f);
    const npyv_f32 invf5 = npyv_setall_f32(0x1.11119ap-07f);
    const npyv_f32 invf3 = npyv_setall_f32(-0x1.555556p-03f);

    npyv_f32 r = npyv_muladd_f32(invf9, x2, invf7);
    r = npyv_muladd_f32(r, x2, invf5);
    r = npyv_muladd_f32(r, x2, invf3);
    r = npyv_muladd_f32(r, x2, npyv_zero_f32());
    r = npyv_muladd_f32(r, x, x);
    return r;
}
/*
 * Vectorized approximate sine/cosine algorithms: The following code is a
 * vectorized version of the algorithm presented here:
 * https://stackoverflow.com/questions/30463616/payne-hanek-algorithm-implementation-in-c/30465751#30465751
 * (1) Load data in registers and generate mask for elements that are
 * within range [-71476.0625f, 71476.0625f] for cosine and [-117435.992f,
 * 117435.992f] for sine.
 * (2) For elements within range, perform range reduction using Cody-Waite's
 * method: x* = x - y*PI/2, where y = rint(x*2/PI). x* \in [-PI/4, PI/4].
 * (3) Map cos(x) to (+/-)sine or (+/-)cosine of x* based on the quadrant k =
 * int(y).
 * (4) For elements outside that range, Cody-Waite reduction performs poorly
 * leading to catastrophic cancellation. We compute cosine by calling glibc in
 * a scalar fashion.
 * (5) Vectorized implementation has a max ULP of 1.49 and performs at least
 * 5-7x(x86) - 2.5-3x(Power) - 1-2x(Arm) faster than scalar implementations
 * when magnitude of all elements in the array < 71476.0625f (117435.992f for sine).
 * Worst case performance is when all the elements are large leading to about 1-2% reduction in
 * performance.
 */
typedef enum
{
    SIMD_COMPUTE_SIN,
    SIMD_COMPUTE_COS
} SIMD_TRIG_OP;

static void SIMD_MSVC_NOINLINE
simd_sincos_f32(const float *src, npy_intp ssrc, float *dst, npy_intp sdst,
                npy_intp len, SIMD_TRIG_OP trig_op)
{
    // Load up frequently used constants
    const npyv_f32 zerosf = npyv_zero_f32();
    const npyv_s32 ones  = npyv_setall_s32(1);
    const npyv_s32 twos  = npyv_setall_s32(2);
    const npyv_f32 two_over_pi = npyv_setall_f32(0x1.45f306p-1f);
    const npyv_f32 codyw_pio2_highf = npyv_setall_f32(-0x1.921fb0p+00f);
    const npyv_f32 codyw_pio2_medf = npyv_setall_f32(-0x1.5110b4p-22f);
    const npyv_f32 codyw_pio2_lowf = npyv_setall_f32(-0x1.846988p-48f);
    const npyv_f32 rint_cvt_magic = npyv_setall_f32(0x1.800000p+23f);
    // Cody-Waite's range
    float max_codi = 117435.992f;
    if (trig_op == SIMD_COMPUTE_COS) {
        max_codi = 71476.0625f;
    }
    const npyv_f32 max_cody = npyv_setall_f32(max_codi);
    const int vstep = npyv_nlanes_f32;

    for (; len > 0; len -= vstep, src += ssrc*vstep, dst += sdst*vstep) {
        npyv_f32 x_in;
        if (ssrc == 1) {
            x_in = npyv_load_tillz_f32(src, len);
        } else {
            x_in = npyv_loadn_tillz_f32(src, ssrc, len);
        }
        npyv_b32 simd_mask = npyv_cmple_f32(npyv_abs_f32(x_in), max_cody);
        npy_uint64 simd_maski = npyv_tobits_b32(simd_mask);
        /*
         * For elements outside of this range, Cody-Waite's range reduction
         * becomes inaccurate and we will call libc to compute cosine for
         * these numbers
         */
        if (simd_maski != 0) {
            npyv_b32 nnan_mask = npyv_notnan_f32(x_in);
            npyv_f32 x = npyv_select_f32(npyv_and_b32(nnan_mask, simd_mask), x_in, zerosf);

            npyv_f32 quadrant = npyv_mul_f32(x, two_over_pi);
            // round to nearest, -0.0f -> +0.0f, and |a| must be <= 0x1.0p+22
            quadrant = npyv_add_f32(quadrant, rint_cvt_magic);
            quadrant = npyv_sub_f32(quadrant, rint_cvt_magic);

            // Cody-Waite's range reduction algorithm
            npyv_f32 reduced_x = simd_range_reduction_f32(
                x, quadrant, codyw_pio2_highf, codyw_pio2_medf, codyw_pio2_lowf
            );
            npyv_f32 reduced_x2 = npyv_square_f32(reduced_x);

            // compute cosine and sine
            npyv_f32 cos = simd_cosine_poly_f32(reduced_x2);
            npyv_f32 sin = simd_sine_poly_f32(reduced_x, reduced_x2);

            npyv_s32 iquadrant = npyv_round_s32_f32(quadrant);
            if (trig_op == SIMD_COMPUTE_COS) {
                iquadrant = npyv_add_s32(iquadrant, ones);
            }
            // blend sin and cos based on the quadrant
            npyv_b32 sine_mask = npyv_cmpeq_s32(npyv_and_s32(iquadrant, ones), npyv_zero_s32());
            cos = npyv_select_f32(sine_mask, sin, cos);

            // multiply by -1 for appropriate elements
            npyv_b32 negate_mask = npyv_cmpeq_s32(npyv_and_s32(iquadrant, twos), twos);
            cos = npyv_ifsub_f32(negate_mask, zerosf, cos, cos);
            cos = npyv_select_f32(nnan_mask, cos, npyv_setall_f32(NPY_NANF));

            if (sdst == 1) {
                npyv_store_till_f32(dst, len, cos);
            } else {
                npyv_storen_till_f32(dst, sdst, len, cos);
            }
        }
        if (simd_maski != ((1 << vstep) - 1)) {
            float NPY_DECL_ALIGNED(NPY_SIMD_WIDTH) ip_fback[npyv_nlanes_f32];
            npyv_storea_f32(ip_fback, x_in);

            // process elements using libc for large elements
            if (trig_op == SIMD_COMPUTE_COS) {
                for (unsigned i = 0; i < npyv_nlanes_f32; ++i) {
                    if ((simd_maski >> i) & 1) {
                        continue;
                    }
                    dst[sdst*i] = npy_cosf(ip_fback[i]);
                }
            }
            else {
                for (unsigned i = 0; i < npyv_nlanes_f32; ++i) {
                    if ((simd_maski >> i) & 1) {
                        continue;
                    }
                    dst[sdst*i] = npy_sinf(ip_fback[i]);
                }
            }
        }
    }
    npyv_cleanup();
}
#endif // NPY_SIMD_FMA3

#line 202
NPY_NO_EXPORT void NPY_CPU_DISPATCH_CURFX(FLOAT_cos)
(char **args, npy_intp const *dimensions, npy_intp const *steps, void *NPY_UNUSED(data))
{
    const float *src = (float*)args[0];
          float *dst = (float*)args[1];

    const int lsize = sizeof(src[0]);
    const npy_intp ssrc = steps[0] / lsize;
    const npy_intp sdst = steps[1] / lsize;
    npy_intp len = dimensions[0];
    assert(len <= 1 || (steps[0] % lsize == 0 && steps[1] % lsize == 0));
#if NPY_SIMD_FMA3
    if (is_mem_overlap(src, steps[0], dst, steps[1], len) ||
        !npyv_loadable_stride_f32(ssrc) || !npyv_storable_stride_f32(sdst)
    ) {
        for (; len > 0; --len, src += ssrc, dst += sdst) {
            simd_sincos_f32(src, 1, dst, 1, 1, SIMD_COMPUTE_COS);
        }
    } else {
        simd_sincos_f32(src, ssrc, dst, sdst, len, SIMD_COMPUTE_COS);
    }
#else
    for (; len > 0; --len, src += ssrc, dst += sdst) {
        const float src0 = *src;
        *dst = npy_cosf(src0);
    }
#endif
}

#line 202
NPY_NO_EXPORT void NPY_CPU_DISPATCH_CURFX(FLOAT_sin)
(char **args, npy_intp const *dimensions, npy_intp const *steps, void *NPY_UNUSED(data))
{
    const float *src = (float*)args[0];
          float *dst = (float*)args[1];

    const int lsize = sizeof(src[0]);
    const npy_intp ssrc = steps[0] / lsize;
    const npy_intp sdst = steps[1] / lsize;
    npy_intp len = dimensions[0];
    assert(len <= 1 || (steps[0] % lsize == 0 && steps[1] % lsize == 0));
#if NPY_SIMD_FMA3
    if (is_mem_overlap(src, steps[0], dst, steps[1], len) ||
        !npyv_loadable_stride_f32(ssrc) || !npyv_storable_stride_f32(sdst)
    ) {
        for (; len > 0; --len, src += ssrc, dst += sdst) {
            simd_sincos_f32(src, 1, dst, 1, 1, SIMD_COMPUTE_SIN);
        }
    } else {
        simd_sincos_f32(src, ssrc, dst, sdst, len, SIMD_COMPUTE_SIN);
    }
#else
    for (; len > 0; --len, src += ssrc, dst += sdst) {
        const float src0 = *src;
        *dst = npy_sinf(src0);
    }
#endif
}


