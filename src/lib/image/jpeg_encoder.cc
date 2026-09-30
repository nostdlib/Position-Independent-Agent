/**
 * @file jpeg_encoder.cc
 * @brief Position-Independent JPEG Encoder Implementation
 *
 * @details Implements baseline JPEG encoding (ITU-T T.81) with the
 * Arai–Agui–Nakajima scaled DCT algorithm. Adapted from Tiny JPEG Encoder
 * by Sergio Gonzalez for position-independent execution.
 *
 * Internal float constants are embedded as UINT32 immediates via
 * __builtin_bit_cast to avoid .rdata section generation.
 *
 * On x86_64 the per-MCU hot loops run through an SSE2 path (baseline on
 * that arch, so no runtime dispatch): the 4:2:0 color conversion and the
 * forward DCT process four blocks per 128-bit lane. Vector types and the
 * unaligned load come from core/compiler/sse2.h; the punpck/pmadd surface
 * stays local, and every vector constant is an integer splat forced
 * through a GPR barrier so the backend can never pool it into .rodata.cst*,
 * which would break the PIC build.
 *
 * @note Original DCT implementation by Thomas G. Lane (via NVIDIA SDK).
 *
 * @see ITU-T T.81 — JPEG standard
 *      https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 * @see Arai, Agui, Nakajima — "A fast DCT-SQ scheme for images"
 *      Trans. IEICE E-71(11):1095, 1988
 */

#include "lib/image/jpeg_encoder.h"
#include "core/memory/memory.h"
#include "core/math/byteorder.h"
#include "core/compiler/sse2.h"

// ============================================================
//  Constants
// ============================================================

static constexpr INT32 BufferSize = 1024;

/// @brief Reinterpret a UINT32 bit pattern as IEEE-754 float
/// @details The register barrier prevents the compiler from constant-folding
/// the bit pattern into a float constant pool (.rdata), which would break
/// position-independence on i386 (no RIP-relative addressing).
static FORCE_INLINE float F32(UINT32 bits)
{
	__asm__ volatile("" : "+r"(bits));
	return __builtin_bit_cast(float, bits);
}

// ============================================================
//  SSE2 hot-loop surface (x86_64 only)
//
//  No compiler headers: the vector types are clang vector extensions and
//  the operations are __builtin_ia32_* intrinsics (SSE2 is baseline on
//  x86_64) plus __builtin_shufflevector, which the backend lowers to the
//  matching single SSE2 instruction. Lane permutes built this way never
//  constant-pool; every data-carrying constant goes through VSplat's GPR
//  barrier below for the same reason.
// ============================================================

// AAN DCT constants shared by the SIMD vector init and the scalar path —
// single-sourced so a retune cannot diverge the two
static constexpr UINT32 kDctC4Bits = 0x3F3504F3u;   ///< cos(4*pi/16) * sqrt(2)
static constexpr UINT32 kDctC6Bits = 0x3EC3EF15u;   ///< cos(6*pi/16) * sqrt(2)
static constexpr UINT32 kDctC2C6Bits = 0x3F0A8BD4u; ///< cos(2*pi/16) - cos(6*pi/16)
static constexpr UINT32 kDctC2P6Bits = 0x3FA73D75u; ///< cos(2*pi/16) + cos(6*pi/16)

#if defined(ARCHITECTURE_X86_64)

/// @brief 128-bit vector of 8-bit lanes
typedef SseVec16b V16B;
/// @brief 128-bit vector of 16-bit lanes
typedef INT16 V8S __attribute__((__vector_size__(16)));
/// @brief 128-bit vector of 32-bit lanes
typedef INT32 V4S __attribute__((__vector_size__(16)));
/// @brief 128-bit vector of float lanes
typedef float V4F __attribute__((__vector_size__(16)));

/// @brief Bit-cast reinterpretation between same-width vector types
template <typename D, typename S>
static FORCE_INLINE D VecCast(S v)
{
	return (D)v;
}

/// @brief Load 16 bytes from an arbitrarily aligned address
static FORCE_INLINE V16B LoadU(const UINT8 *p)
{
	return SseLoadU(p);
}

/// @brief Unpack low bytes of two vectors into interleaved byte lanes
static FORCE_INLINE V16B PunpckLBW(V16B a, V16B b)
{
	return __builtin_shufflevector(a, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
}

/// @brief Unpack high bytes of two vectors into interleaved byte lanes
static FORCE_INLINE V16B PunpckHBW(V16B a, V16B b)
{
	return __builtin_shufflevector(a, b, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);
}

/// @brief Unpack low 16-bit lanes of two vectors, interleaved
static FORCE_INLINE V8S PunpckLWD(V8S a, V8S b)
{
	return __builtin_shufflevector(a, b, 0, 8, 1, 9, 2, 10, 3, 11);
}

/// @brief Unpack high 16-bit lanes of two vectors, interleaved
static FORCE_INLINE V8S PunpckHWD(V8S a, V8S b)
{
	return __builtin_shufflevector(a, b, 4, 12, 5, 13, 6, 14, 7, 15);
}

/// @brief Unpack low 32-bit lanes of two vectors, interleaved
static FORCE_INLINE V4S PunpckLDQ(V4S a, V4S b)
{
	return __builtin_shufflevector(a, b, 0, 4, 1, 5);
}

/// @brief Unpack high 32-bit lanes of two vectors, interleaved
static FORCE_INLINE V4S PunpckHDQ(V4S a, V4S b)
{
	return __builtin_shufflevector(a, b, 2, 6, 3, 7);
}

/// @brief Unpack low 64-bit halves of two vectors, interleaved
static FORCE_INLINE V4S PunpckLQDQ(V4S a, V4S b)
{
	return __builtin_shufflevector(a, b, 0, 1, 4, 5);
}

/// @brief Unpack high 64-bit halves of two vectors, interleaved
static FORCE_INLINE V4S PunpckHQDQ(V4S a, V4S b)
{
	return __builtin_shufflevector(a, b, 2, 3, 6, 7);
}

/// @brief Byte-granular left shift of the whole register by 8 (pslldq)
static FORCE_INLINE V16B Slli128(V16B v)
{
	V16B z = v ^ v;
	return __builtin_shufflevector(z, v, 0, 0, 0, 0, 0, 0, 0, 0, 16, 17, 18, 19, 20, 21, 22, 23);
}

/// @brief Byte-granular right shift of the whole register by 8 (psrldq)
static FORCE_INLINE V16B Srli128(V16B v)
{
	V16B z = v ^ v;
	return __builtin_shufflevector(v, z, 8, 9, 10, 11, 12, 13, 14, 15, 16, 16, 16, 16, 16, 16, 16, 16);
}

/// @brief Multiply adjacent signed 16-bit lane pairs, summing into 32-bit lanes
static FORCE_INLINE V4S PmaddWD(V8S a, V8S b)
{
	return __builtin_ia32_pmaddwd128(a, b);
}

/// @brief Arithmetic (sign-propagating) right shift of 32-bit lanes
static FORCE_INLINE V4S Psrad(V4S v, INT32 bits)
{
	return __builtin_ia32_psradi128(v, bits);
}

/// @brief Logical right shift of 32-bit lanes
static FORCE_INLINE V4S Psrld(V4S v, INT32 bits)
{
	return __builtin_ia32_psrldi128(v, bits);
}

/// @brief Logical right shift of 16-bit lanes
static FORCE_INLINE V8S Psrlw(V8S v, INT32 bits)
{
	return __builtin_ia32_psrlwi128(v, bits);
}

/// @brief Pack 32-bit lanes to 16-bit lanes with signed saturation
static FORCE_INLINE V8S Packssdw(V4S a, V4S b)
{
	return __builtin_ia32_packssdw128(a, b);
}

/// @brief Convert 32-bit integer lanes to float lanes
static FORCE_INLINE V4F Cvtdq2ps(V4S v)
{
	return __builtin_convertvector(v, V4F);
}

/// @brief Convert float lanes to 32-bit integers (round to nearest even)
static FORCE_INLINE V4S Cvtps2dq(V4F v)
{
	return __builtin_ia32_cvtps2dq(v);
}

/// @brief Splat a 32-bit pattern to all lanes without a constant-pool load
/// @details The GPR register barrier hides the value from the optimizer, so
/// the splat is always materialized as movd + pshufd from a GPR immediate —
/// never a rip-relative .rodata.cst16 vector load
static FORCE_INLINE V4S VSplat(UINT32 bits)
{
	__asm__ volatile("" : "+r"(bits));
	V4S v = {(INT32)bits, (INT32)bits, (INT32)bits, (INT32)bits};
	return __builtin_shufflevector(v, v, 0, 0, 0, 0);
}

// ============================================================
//  SSE2 constants, materialized once per encode
// ============================================================

/// @brief Hot-path SSE2 vector constants, built once per encode
/// @details The (R,G)/(B,G) weight-pair split keeps every pmaddwd
/// coefficient inside int16: 0.587 = 0.337 + 0.250 (libjpeg-turbo's trick)
struct Sse2Const
{
	V8S yRG;		 ///< [19595, 22086] per (R,G) pair: 0.299R + (0.587-0.25)G
	V8S yBG;		 ///< [7471, 16384] per (B,G) pair: 0.114B + 0.250G
	V8S cbRG;		 ///< [-11059, -21712] per (r,g) pair
	V8S crGB;		 ///< [-27441, -5328] per (g,b) pair
	V4F dctDcBias;	 ///< 8192 << 16, the unfolded -128 level shift at DC
	V8S chromaHalf; ///< +2 rounding for the 2x2 box average
	V4S chromaBias; ///< +32768 rounding before the >> 16
	V4F dctC4;		 ///< cos(4*pi/16) * sqrt(2)
	V4F dctC6;		 ///< cos(6*pi/16) * sqrt(2)
	V4F dctC2C6;	 ///< cos(2*pi/16) - cos(6*pi/16)
	V4F dctC2P6;	 ///< cos(2*pi/16) + cos(6*pi/16)
};

/// @brief Initialize the per-encode SSE2 constant set
static VOID InitSse2Const(Sse2Const *k)
{
	k->yRG = VecCast<V8S>(VSplat(19595u | (22086u << 16)));
	k->yBG = VecCast<V8S>(VSplat(7471u | (16384u << 16)));
	k->cbRG = VecCast<V8S>(VSplat((UINT32)(-11059 & 0xFFFF) | ((UINT32)(-21712 & 0xFFFF) << 16)));
	k->crGB = VecCast<V8S>(VSplat((UINT32)(-27441 & 0xFFFF) | ((UINT32)(-5328 & 0xFFFF) << 16)));
	k->dctDcBias = VecCast<V4F>(VSplat(0x4E000000u)); // 8192 << 16
	k->chromaHalf = VecCast<V8S>(VSplat(0x00020002u));
	k->chromaBias = VSplat(32768u);
	k->dctC4 = VecCast<V4F>(VSplat(kDctC4Bits));
	k->dctC6 = VecCast<V4F>(VSplat(kDctC6Bits));
	k->dctC2C6 = VecCast<V4F>(VSplat(kDctC2C6Bits));
	k->dctC2P6 = VecCast<V4F>(VSplat(kDctC2P6Bits));
}

// ============================================================
//  RGB deinterleave: 16 pixels -> even/odd channel planes (u16 x8)
// ============================================================

/// @brief The six per-row channel planes the SIMD pipeline works on
struct RowPlanes
{
	V8S re, ro; ///< red of even/odd pixels
	V8S ge, go; ///< green of even/odd pixels
	V8S be, bo; ///< blue of even/odd pixels
};

/// @brief Deinterleave 16 RGB (3 bytes/px) pixels into channel planes
/// @details Punpck/shift network per libjpeg-turbo jccolext-sse2: three
///          16-byte loads cover the 48 source bytes, so no read ever
///          crosses past the row (the caller guarantees mcuX + 16 <= width)
static VOID DeinterleaveRgb3(const UINT8 *p, RowPlanes *r)
{
	V16B a = LoadU(p);
	V16B f = LoadU(p + 16);
	V16B b = LoadU(p + 32);

	V16B g = a;
	a = Slli128(a);
	g = Srli128(g);
	a = PunpckHBW(a, f);
	f = Slli128(f);
	g = PunpckLBW(g, b);
	f = PunpckHBW(f, b);

	V16B d = a;
	a = Slli128(a);
	d = Srli128(d);
	a = PunpckHBW(a, g);
	g = Slli128(g);
	d = PunpckLBW(d, f);
	g = PunpckHBW(g, f);

	V16B e = a;
	a = Slli128(a);
	e = Srli128(e);
	a = PunpckHBW(a, d);
	d = Slli128(d);
	e = PunpckLBW(e, g);
	d = PunpckHBW(d, g);

	V16B z = a ^ a;
	r->re = VecCast<V8S>(PunpckLBW(a, z));
	r->ge = VecCast<V8S>(PunpckHBW(a, z));
	r->be = VecCast<V8S>(PunpckLBW(e, z));
	r->ro = VecCast<V8S>(PunpckHBW(e, z));
	r->go = VecCast<V8S>(PunpckLBW(d, z));
	r->bo = VecCast<V8S>(PunpckHBW(d, z));
}

/// @brief Deinterleave 16 RGBA (4 bytes/px) pixels into channel planes
/// @details Each 32-bit lane holds one full pixel; even/odd pixels are the
///          even/odd lanes and channel bytes come off with uniform shifts.
///          Values are 0..255, so the packssdw compaction is exact
static VOID DeinterleaveRgba4(const UINT8 *p, RowPlanes *r)
{
	V4S v0 = VecCast<V4S>(LoadU(p));
	V4S v1 = VecCast<V4S>(LoadU(p + 16));
	V4S v2 = VecCast<V4S>(LoadU(p + 32));
	V4S v3 = VecCast<V4S>(LoadU(p + 48));

	V4S g0 = Psrld(v0, 8), g1 = Psrld(v1, 8);
	V4S g2 = Psrld(v2, 8), g3 = Psrld(v3, 8);
	V4S b0 = Psrld(v0, 16), b1 = Psrld(v1, 16);
	V4S b2 = Psrld(v2, 16), b3 = Psrld(v3, 16);

	V4S byte = VSplat(0x000000FFu);

	r->re = Packssdw(__builtin_shufflevector(v0, v1, 0, 2, 4, 6) & byte,
					 __builtin_shufflevector(v2, v3, 0, 2, 4, 6) & byte);
	r->ro = Packssdw(__builtin_shufflevector(v0, v1, 1, 3, 5, 7) & byte,
					 __builtin_shufflevector(v2, v3, 1, 3, 5, 7) & byte);
	r->ge = Packssdw(__builtin_shufflevector(g0, g1, 0, 2, 4, 6) & byte,
					 __builtin_shufflevector(g2, g3, 0, 2, 4, 6) & byte);
	r->go = Packssdw(__builtin_shufflevector(g0, g1, 1, 3, 5, 7) & byte,
					 __builtin_shufflevector(g2, g3, 1, 3, 5, 7) & byte);
	r->be = Packssdw(__builtin_shufflevector(b0, b1, 0, 2, 4, 6) & byte,
					 __builtin_shufflevector(b2, b3, 0, 2, 4, 6) & byte);
	r->bo = Packssdw(__builtin_shufflevector(b0, b1, 1, 3, 5, 7) & byte,
					 __builtin_shufflevector(b2, b3, 1, 3, 5, 7) & byte);
}

// ============================================================
//  4-wide float DCT (lane = block), AAN butterflies
// ============================================================

/// @brief Forward 8x8 DCT over four blocks held lane-interleaved
/// @details data[i] lane q = sample i of block q. The butterfly structure
///          is identical to the scalar AAN DCT with every operation
///          lane-vertical, so all four blocks advance together. The second
///          pass multiplies each output by its (broadcast) quantize factor
///          and converts to integer lanes in place, saving the separate
///          64-element quantize sweep. dcBias carries the unfolded -128
///          level shift of the luma path (8192.0f) or zero for chroma
static VOID ForwardDCTQuantize4(V4F *data, const V4F *pqt4, V4S *out, V4F dcBias, const Sse2Const *k)
{
	V4F c4 = k->dctC4;
	V4F c6 = k->dctC6;
	V4F c2c6 = k->dctC2C6;
	V4F c2p6 = k->dctC2P6;

	// Pass 1: process rows
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		V4F *d = data + ctr * 8;
		V4F t0 = d[0] + d[7], t7 = d[0] - d[7];
		V4F t1 = d[1] + d[6], t6 = d[1] - d[6];
		V4F t2 = d[2] + d[5], t5 = d[2] - d[5];
		V4F t3 = d[3] + d[4], t4 = d[3] - d[4];

		V4F t10 = t0 + t3, t13 = t0 - t3;
		V4F t11 = t1 + t2, t12 = t1 - t2;

		d[0] = t10 + t11;
		d[4] = t10 - t11;

		V4F z1 = (t12 + t13) * c4;
		d[2] = t13 + z1;
		d[6] = t13 - z1;

		t10 = t4 + t5;
		t11 = t5 + t6;
		t12 = t6 + t7;

		V4F z5 = (t10 - t12) * c6;
		V4F z2 = c2c6 * t10 + z5;
		V4F z4 = c2p6 * t12 + z5;
		V4F z3 = t11 * c4;

		V4F z11 = t7 + z3;
		V4F z13 = t7 - z3;

		d[5] = z13 + z2;
		d[3] = z13 - z2;
		d[1] = z11 + z4;
		d[7] = z11 - z4;
	}

	// Pass 2: process columns, fusing the quantize multiply and the
	// integer conversion into the eight output stores
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		V4F *d = data + ctr;
		V4S *o = out + ctr;
		V4F t0 = d[8 * 0] + d[8 * 7], t7 = d[8 * 0] - d[8 * 7];
		V4F t1 = d[8 * 1] + d[8 * 6], t6 = d[8 * 1] - d[8 * 6];
		V4F t2 = d[8 * 2] + d[8 * 5], t5 = d[8 * 2] - d[8 * 5];
		V4F t3 = d[8 * 3] + d[8 * 4], t4 = d[8 * 3] - d[8 * 4];

		V4F t10 = t0 + t3, t13 = t0 - t3;
		V4F t11 = t1 + t2, t12 = t1 - t2;

		// Only DC carries the unfolded level shift; other coefficients of a
		// constant image are zero, so one correction on the (0,0) output
		V4F dc0 = t10 + t11;
		if (ctr == 0)
			dc0 = dc0 - dcBias;
		o[8 * 0] = Cvtps2dq(dc0 * pqt4[8 * 0 + ctr]);
		o[8 * 4] = Cvtps2dq((t10 - t11) * pqt4[8 * 4 + ctr]);

		V4F z1 = (t12 + t13) * c4;
		o[8 * 2] = Cvtps2dq((t13 + z1) * pqt4[8 * 2 + ctr]);
		o[8 * 6] = Cvtps2dq((t13 - z1) * pqt4[8 * 6 + ctr]);

		t10 = t4 + t5;
		t11 = t5 + t6;
		t12 = t6 + t7;

		V4F z5 = (t10 - t12) * c6;
		V4F z2 = c2c6 * t10 + z5;
		V4F z4 = c2p6 * t12 + z5;
		V4F z3 = t11 * c4;

		V4F z11 = t7 + z3;
		V4F z13 = t7 - z3;

		o[8 * 5] = Cvtps2dq((z13 + z2) * pqt4[8 * 5 + ctr]);
		o[8 * 3] = Cvtps2dq((z13 - z2) * pqt4[8 * 3 + ctr]);
		o[8 * 1] = Cvtps2dq((z11 + z4) * pqt4[8 * 1 + ctr]);
		o[8 * 7] = Cvtps2dq((z11 - z4) * pqt4[8 * 7 + ctr]);
	}
}

/// @brief Transpose 64 lane-interleaved vectors into zig-zag-ordered du
/// @details src[i] lane q = coefficient i of block q. The 4x4 transposes
///          land natural coefficient order in registers; each lane store
///          routes through zigZag directly into the entropy stage's du,
///          replacing the separate per-block reordering scatter. blocks
///          selects how many lanes carry real blocks (4 luma, 2 chroma)
static VOID Transpose4Zig(const V4S *src, INT32 *du0, INT32 *du1, INT32 *du2, INT32 *du3,
						  INT32 blocks, const UINT8 *zigZag)
{
	for (INT32 t = 0; t < 16; ++t)
	{
		V4S v0 = src[t * 4 + 0];
		V4S v1 = src[t * 4 + 1];
		V4S v2 = src[t * 4 + 2];
		V4S v3 = src[t * 4 + 3];

		V4S l01 = PunpckLDQ(v0, v1);
		V4S h01 = PunpckHDQ(v0, v1);
		V4S l23 = PunpckLDQ(v2, v3);
		V4S h23 = PunpckHDQ(v2, v3);

		V4S c0 = PunpckLQDQ(l01, l23);
		V4S c1 = PunpckHQDQ(l01, l23);
		V4S c2 = PunpckLQDQ(h01, h23);
		V4S c3 = PunpckHQDQ(h01, h23);

		INT32 zz0 = zigZag[t * 4 + 0];
		INT32 zz1 = zigZag[t * 4 + 1];
		INT32 zz2 = zigZag[t * 4 + 2];
		INT32 zz3 = zigZag[t * 4 + 3];
		du0[zz0] = c0[0];
		du1[zz0] = c1[0];
		du0[zz1] = c0[1];
		du1[zz1] = c1[1];
		du0[zz2] = c0[2];
		du1[zz2] = c1[2];
		du0[zz3] = c0[3];
		du1[zz3] = c1[3];
		if (blocks > 2)
		{
			du2[zz0] = c2[0];
			du2[zz1] = c2[1];
			du2[zz2] = c2[2];
			du2[zz3] = c2[3];
			du3[zz0] = c3[0];
			du3[zz1] = c3[1];
			du3[zz2] = c3[2];
			du3[zz3] = c3[3];
		}
	}
}

#endif // ARCHITECTURE_X86_64

// ============================================================
//  Internal types
// ============================================================

/// @brief Context for the user-provided write callback
struct WriteContext
{
	PVOID context;
	JpegWriteFunc *func;
};

/// @brief Internal encoder state for a single encode operation
struct EncoderState
{
	UINT8 ehuffsize[4][257];
	UINT16 ehuffcode[4][256];
	const UINT8 *htBits[4];
	const UINT8 *htVals[4];

	UINT8 qtLuma[64];
	UINT8 qtChroma[64];

	WriteContext writeContext;

	USIZE outputBufferCount;
	UINT8 outputBuffer[BufferSize];
};

/// @brief Pre-processed quantization matrices (1/divisor for multiplication)
struct ProcessedQT
{
	float chroma[64];
	float luma[64];
};

/// @brief Hot-path float constants, materialized once per encode
/// @details The F32() volatile register barrier can be neither hoisted nor
/// CSE-ed, so per-use materialization inside pixel/block loops pays a
/// GPR->XMM transfer every iteration. One stack struct built per Encode
/// lets -O3 keep the values in registers across the hot loops.
struct EncodeConstants
{
	float dctC4;	 ///< cos(4*pi/16) * sqrt(2) = 0.707106781
	float dctC6;	 ///< cos(6*pi/16) * sqrt(2) = 0.382683433
	float dctC2C6;	 ///< cos(2*pi/16) - cos(6*pi/16) = 0.541196100
	float dctC2P6;	 ///< cos(2*pi/16) + cos(6*pi/16) = 1.306562965
	float quantBias; ///< 1024.0f, half-up rounding bias
	float quantHalf; ///< 0.5f
	float lumaScale; ///< 2^-16, 16.16 fixed-point to float
	float neg128;	 ///< -128.0f level shift (negated so FMA contraction needs no 0x80000000 constant-pool splat)
	float yccR;		 ///< 0.299f
	float yccG;		 ///< 0.587f
	float yccB;		 ///< 0.114f
	float yccCbR;	 ///< -0.1687f
	float yccCbG;	 ///< 0.3313f (negative in formula)
	float yccCbB;	 ///< 0.5f
	float yccCrR;	 ///< 0.5f
	float yccCrG;	 ///< 0.4187f (negative in formula)
	float yccCrB;	 ///< 0.0813f (negative in formula)
};

// ============================================================
//  Wire-format JPEG segment headers (packed for exact layout)
// ============================================================

#pragma pack(push)
#pragma pack(1)

/// @brief JFIF APP0 header (ITU-T T.81 Annex B, JFIF 1.02)
struct JFIFHeader
{
	UINT16 SOI;
	UINT16 APP0;
	UINT16 jfifLen;
	UINT8 jfifId[5];
	UINT16 version;
	UINT8 units;
	UINT16 xDensity;
	UINT16 yDensity;
	UINT8 xThumb;
	UINT8 yThumb;
};

/// @brief Component specification within SOF marker (ITU-T T.81 A.1.1)
struct ComponentSpec
{
	UINT8 componentId;
	UINT8 samplingFactors; ///< Upper 4 bits: horizontal, lower 4: vertical
	UINT8 qt;			   ///< Quantization table selector
};

/// @brief SOF0 frame header (ITU-T T.81 B.2.2)
struct FrameHeader
{
	UINT16 SOF;
	UINT16 len;
	UINT8 precision;
	UINT16 height;
	UINT16 width;
	UINT8 numComponents;
	ComponentSpec componentSpec[3];
};

/// @brief Component specification within SOS marker
struct ScanComponentSpec
{
	UINT8 componentId;
	UINT8 dcAc; ///< (DC table selector << 4) | AC table selector
};

/// @brief SOS scan header (ITU-T T.81 B.2.3)
struct ScanHeader
{
	UINT16 SOS;
	UINT16 len;
	UINT8 numComponents;
	ScanComponentSpec componentSpec[3];
	UINT8 first;
	UINT8 last;
	UINT8 ahAl;
};

#pragma pack(pop)

// ============================================================
//  Huffman table class indices
// ============================================================

enum HuffmanTableIndex : INT32
{
	LumaDC = 0,
	LumaAC = 1,
	ChromaDC = 2,
	ChromaAC = 3,
};

enum HuffmanTableClass : INT32
{
	DC = 0,
	AC = 1,
};

// ============================================================
//  Zig-zag order (ITU-T T.81 Figure A.6)
//  Stack-local to avoid .rodata generation.
// ============================================================

/// @brief Initialize the zig-zag reordering table on the stack
static VOID InitZigZag(UINT8 zz[64])
{
	const UINT8 embedded[] = {
		0, 1, 5, 6, 14, 15, 27, 28,
		2, 4, 7, 13, 16, 26, 29, 42,
		3, 8, 12, 17, 25, 30, 41, 43,
		9, 11, 18, 24, 31, 40, 44, 53,
		10, 19, 23, 32, 39, 45, 52, 54,
		20, 22, 33, 38, 46, 51, 55, 60,
		21, 34, 37, 47, 50, 56, 59, 61,
		35, 36, 48, 49, 57, 58, 62, 63};
	Memory::Copy(zz, embedded, 64);
}

// ============================================================
//  Buffered output
// ============================================================

/**
 * @brief Write data to the output buffer, flushing when full
 *
 * @details Buffers output in chunks of BufferSize-1 bytes before flushing
 * to the user callback. Recursively handles writes larger than the buffer.
 *
 * @param state Encoder state with output buffer
 * @param data Data to write
 * @param numBytes Number of bytes to write
 */
static VOID WriteOutput(EncoderState *state, const PVOID data, USIZE numBytes)
{
	USIZE capped = numBytes;
	if (capped > BufferSize - 1 - state->outputBufferCount)
		capped = BufferSize - 1 - state->outputBufferCount;

	Memory::Copy(state->outputBuffer + state->outputBufferCount, data, capped);
	state->outputBufferCount += capped;

	if (state->outputBufferCount == (USIZE)(BufferSize - 1))
	{
		state->writeContext.func(state->writeContext.context, state->outputBuffer, (INT32)state->outputBufferCount);
		state->outputBufferCount = 0;
	}

	if (capped < numBytes)
	{
		WriteOutput(state, (UINT8 *)data + capped, numBytes - capped);
	}
}

// ============================================================
//  JPEG marker writing
// ============================================================

/**
 * @brief Write a DQT (Define Quantization Table) marker
 *
 * @param state Encoder state
 * @param matrix 64-byte quantization table in zig-zag order
 * @param id Table destination identifier (0 or 1)
 *
 * @see ITU-T T.81 B.2.4.1 — Quantization table-specification syntax
 */
static VOID WriteDQT(EncoderState *state, const UINT8 *matrix, UINT8 id)
{
	UINT16 marker = ByteOrder::Swap16(0xFFDB);
	WriteOutput(state, &marker, sizeof(UINT16));
	UINT16 len = ByteOrder::Swap16(0x0043); // 2 + 1 + 64 = 67
	WriteOutput(state, &len, sizeof(UINT16));
	UINT8 precisionAndId = id;
	WriteOutput(state, &precisionAndId, sizeof(UINT8));
	WriteOutput(state, (PVOID)matrix, 64);
}

/**
 * @brief Write a DHT (Define Huffman Table) marker
 *
 * @param state Encoder state
 * @param bits BITS array (16 entries: count of codes per length)
 * @param vals HUFFVAL array (symbol values)
 * @param htClass DC (0) or AC (1) table class
 * @param id Table destination identifier
 *
 * @see ITU-T T.81 B.2.4.2 — Huffman table-specification syntax
 */
static VOID WriteDHT(EncoderState *state, const UINT8 *bits, const UINT8 *vals,
					 HuffmanTableClass htClass, UINT8 id)
{
	INT32 numValues = 0;
	for (INT32 i = 0; i < 16; ++i)
		numValues += bits[i];

	UINT16 marker = ByteOrder::Swap16(0xFFC4);
	UINT16 len = ByteOrder::Swap16((UINT16)(2 + 1 + 16 + numValues));
	UINT8 tcTh = (UINT8)(((UINT8)htClass << 4) | id);

	WriteOutput(state, &marker, sizeof(UINT16));
	WriteOutput(state, &len, sizeof(UINT16));
	WriteOutput(state, &tcTh, sizeof(UINT8));
	WriteOutput(state, (PVOID)bits, 16);
	WriteOutput(state, (PVOID)vals, (USIZE)numValues);
}

// ============================================================
//  Huffman code generation (ITU-T T.81 Annex C)
// ============================================================

/**
 * @brief Generate code sizes from BITS specification
 *
 * @details Implements procedure specified in ITU-T T.81 Figure C.1.
 * Generates HUFFSIZE table from the BITS counts.
 *
 * @param huffsize Output array (at least 257 entries)
 * @param bits BITS array (16 entries)
 *
 * @see ITU-T T.81 C.2 — Generation of table of Huffman code sizes
 */
static VOID GenerateCodeLengths(UINT8 huffsize[/*257*/], const UINT8 *bits)
{
	INT32 k = 0;
	for (INT32 i = 0; i < 16; ++i)
	{
		for (INT32 j = 0; j < bits[i]; ++j)
			huffsize[k++] = (UINT8)(i + 1);
		huffsize[k] = 0;
	}
}

/**
 * @brief Generate Huffman code values from code sizes
 *
 * @details Implements procedure specified in ITU-T T.81 Figure C.2.
 * Generates HUFFCODE table from HUFFSIZE.
 *
 * @param codes Output code array
 * @param huffsize Code size array (terminated by 0)
 *
 * @see ITU-T T.81 C.2 — Generation of table of Huffman codes
 */
static VOID GenerateCodes(UINT16 codes[], const UINT8 *huffsize)
{
	UINT16 code = 0;
	INT32 k = 0;
	UINT8 sz = huffsize[0];
	for (;;)
	{
		do
		{
			codes[k++] = code++;
		} while (huffsize[k] == sz);

		if (huffsize[k] == 0)
			return;

		do
		{
			code = (UINT16)(code << 1);
			++sz;
		} while (huffsize[k] != sz);
	}
}

/**
 * @brief Build extended Huffman tables for encoding
 *
 * @details Maps symbol values to their corresponding codes and sizes
 * for O(1) lookup during entropy coding.
 *
 * @param outEhuffsize Output: code size indexed by symbol value
 * @param outEhuffcode Output: code value indexed by symbol value
 * @param huffval Symbol value table
 * @param huffsize Code size table
 * @param huffcode Code value table
 * @param count Number of entries
 *
 * @see ITU-T T.81 C.2 — Ordering procedure for encoding
 */
static VOID BuildExtendedTable(UINT8 *outEhuffsize, UINT16 *outEhuffcode,
							   const UINT8 *huffval, const UINT8 *huffsize, const UINT16 *huffcode, INT32 count)
{
	for (INT32 k = 0; k < count; ++k)
	{
		UINT8 val = huffval[k];
		outEhuffcode[val] = huffcode[k];
		outEhuffsize[val] = huffsize[k];
	}
}

// ============================================================
//  Entropy coding helpers
// ============================================================

/**
 * @brief Compute variable-length integer encoding for a DCT coefficient
 *
 * @details Implements the SSSS category and additional bits encoding
 * per ITU-T T.81 Table F.1.
 *
 * @param value DCT coefficient value
 * @param out Output: out[0] = additional bits, out[1] = bit count (SSSS category)
 *
 * @see ITU-T T.81 F.1.2.1.1 — Structure of DC code table
 */
static VOID CalculateVLI(INT32 value, UINT16 out[2])
{
	INT32 absVal = value;
	if (value < 0)
	{
		absVal = -absVal;
		--value;
	}
	// Bit length of absVal via a shift ladder — same result as the old
	// per-bit loop (out[1] >= 1, callers only pass nonzero values)
	UINT16 n = 1;
	if (absVal > 0xFFFF)
	{
		absVal >>= 16;
		n = (UINT16)(n + 16);
	}
	if (absVal > 0xFF)
	{
		absVal >>= 8;
		n = (UINT16)(n + 8);
	}
	if (absVal > 0xF)
	{
		absVal >>= 4;
		n = (UINT16)(n + 4);
	}
	if (absVal > 0x3)
	{
		absVal >>= 2;
		n = (UINT16)(n + 2);
	}
	if (absVal > 0x1)
		n = (UINT16)(n + 1);
	out[1] = n;
	out[0] = (UINT16)(value & ((1 << n) - 1));
}

/**
 * @brief Write bits to the output bitstream
 *
 * @details Accumulates bits in a 64-bit buffer; once 32 bits are pending the
 * four completed bytes are emitted in one batched write, with byte-stuffing
 * (0x00 after 0xFF) applied per ITU-T T.81 B.1.1.5 only at flush time.
 *
 * @param state Encoder state
 * @param bitbuffer Current bit accumulator
 * @param location Current bit position in the accumulator (< 32 on return)
 * @param numBits Number of bits to write (1–27: a Huffman code plus an
 *        11-bit VLI suffix fits one call)
 * @param bits Bit values to write (right-aligned)
 *
 * @see ITU-T T.81 B.1.1.5 — Byte stuffing
 */
static VOID WriteBits(EncoderState *state, UINT64 *bitbuffer, UINT32 *location,
					  UINT16 numBits, UINT32 bits)
{
	*bitbuffer |= (UINT64)bits << (64 - (*location + numBits));
	*location += numBits;
	if (*location < 32)
		return;

	UINT8 out[8];
	UINT32 n = 0;
	UINT64 buf = *bitbuffer;
	for (INT32 i = 0; i < 4; ++i)
	{
		UINT8 c = (UINT8)(buf >> 56);
		buf <<= 8;
		out[n++] = c;
		if (c == 0xFF)
			out[n++] = 0;
	}
	WriteOutput(state, out, n);
	*bitbuffer = buf;
	*location -= 32;
}

/**
 * @brief Drain pending whole bytes from the bit accumulator at scan end
 *
 * @param state Encoder state
 * @param bitbuffer Bit accumulator (left with < 8 bits on return)
 * @param location Bit position in the accumulator
 */
static VOID FlushBitBuffer(EncoderState *state, UINT64 *bitbuffer, UINT32 *location)
{
	UINT8 out[8];
	UINT32 n = 0;
	UINT32 bytes = *location >> 3;
	UINT64 buf = *bitbuffer;
	for (UINT32 i = 0; i < bytes; ++i)
	{
		UINT8 c = (UINT8)(buf >> 56);
		buf <<= 8;
		out[n++] = c;
		if (c == 0xFF)
			out[n++] = 0;
	}
	if (n > 0)
		WriteOutput(state, out, n);
	*location &= 7;
}

// ============================================================
//  DCT (Arai–Agui–Nakajima scaled algorithm)
// ============================================================

/**
 * @brief Forward 8x8 DCT using the Arai–Agui–Nakajima algorithm
 *
 * @details Performs a scaled 2D forward DCT on an 8x8 block of float samples.
 * The scaling factors are absorbed into the quantization step, so the output
 * requires division by the scaled quantization matrix rather than the standard one.
 *
 * @param data 64-element float array (8x8 block, row-major)
 * @param c Encode constants (AAN butterfly factors)
 *
 * @see Arai, Agui, Nakajima — Trans. IEICE E-71(11):1095, 1988
 * @see Pennebaker & Mitchell — JPEG: Still Image Data Compression Standard, Figure 4-8
 */
static VOID ForwardDCT(float *data, const EncodeConstants *c)
{
	float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
	float tmp10, tmp11, tmp12, tmp13;
	float z1, z2, z3, z4, z5, z11, z13;

	float c4 = c->dctC4;
	float c6 = c->dctC6;
	float c2c6 = c->dctC2C6;
	float c2p6 = c->dctC2P6;

	// Pass 1: process rows
	float *dataptr = data;
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		tmp0 = dataptr[0] + dataptr[7];
		tmp7 = dataptr[0] - dataptr[7];
		tmp1 = dataptr[1] + dataptr[6];
		tmp6 = dataptr[1] - dataptr[6];
		tmp2 = dataptr[2] + dataptr[5];
		tmp5 = dataptr[2] - dataptr[5];
		tmp3 = dataptr[3] + dataptr[4];
		tmp4 = dataptr[3] - dataptr[4];

		tmp10 = tmp0 + tmp3;
		tmp13 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp12 = tmp1 - tmp2;

		dataptr[0] = tmp10 + tmp11;
		dataptr[4] = tmp10 - tmp11;

		z1 = (tmp12 + tmp13) * c4;
		dataptr[2] = tmp13 + z1;
		dataptr[6] = tmp13 - z1;

		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;

		z5 = (tmp10 - tmp12) * c6;
		z2 = c2c6 * tmp10 + z5;
		z4 = c2p6 * tmp12 + z5;
		z3 = tmp11 * c4;

		z11 = tmp7 + z3;
		z13 = tmp7 - z3;

		dataptr[5] = z13 + z2;
		dataptr[3] = z13 - z2;
		dataptr[1] = z11 + z4;
		dataptr[7] = z11 - z4;

		dataptr += 8;
	}

	// Pass 2: process columns
	dataptr = data;
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		tmp0 = dataptr[8 * 0] + dataptr[8 * 7];
		tmp7 = dataptr[8 * 0] - dataptr[8 * 7];
		tmp1 = dataptr[8 * 1] + dataptr[8 * 6];
		tmp6 = dataptr[8 * 1] - dataptr[8 * 6];
		tmp2 = dataptr[8 * 2] + dataptr[8 * 5];
		tmp5 = dataptr[8 * 2] - dataptr[8 * 5];
		tmp3 = dataptr[8 * 3] + dataptr[8 * 4];
		tmp4 = dataptr[8 * 3] - dataptr[8 * 4];

		tmp10 = tmp0 + tmp3;
		tmp13 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp12 = tmp1 - tmp2;

		dataptr[8 * 0] = tmp10 + tmp11;
		dataptr[8 * 4] = tmp10 - tmp11;

		z1 = (tmp12 + tmp13) * c4;
		dataptr[8 * 2] = tmp13 + z1;
		dataptr[8 * 6] = tmp13 - z1;

		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;

		z5 = (tmp10 - tmp12) * c6;
		z2 = c2c6 * tmp10 + z5;
		z4 = c2p6 * tmp12 + z5;
		z3 = tmp11 * c4;

		z11 = tmp7 + z3;
		z13 = tmp7 - z3;

		dataptr[8 * 5] = z13 + z2;
		dataptr[8 * 3] = z13 - z2;
		dataptr[8 * 1] = z11 + z4;
		dataptr[8 * 7] = z11 - z4;

		dataptr++;
	}
}

// ============================================================
//  MCU encoding
// ============================================================

static VOID EncodeEntropy(EncoderState *state, const INT32 *du,
						  UINT8 *huffDcLen, UINT16 *huffDcCode,
						  UINT8 *huffAcLen, UINT16 *huffAcCode,
						  INT32 *pred, UINT64 *bitbuffer, UINT32 *location);

/**
 * @brief Encode and write a single 8x8 Minimum Coded Unit
 *
 * @details Applies forward DCT, quantizes coefficients using the scaled
 * quantization matrix, then entropy-codes the DC (differential) and AC
 * (run-length) coefficients using Huffman tables.
 *
 * @param state Encoder state
 * @param mcu 64-element float array of sample values (level-shifted by -128)
 * @param qt Pre-processed quantization matrix (1/divisor values)
 * @param huffDcLen DC Huffman code sizes
 * @param huffDcCode DC Huffman code values
 * @param huffAcLen AC Huffman code sizes
 * @param huffAcCode AC Huffman code values
 * @param c Encode constants (DCT factors, quantize rounding bias)
 * @param pred Previous DC coefficient (updated on return)
 * @param bitbuffer Bit accumulator (updated on return)
 * @param location Bit position in accumulator (updated on return)
 *
 * @see ITU-T T.81 F.1.2 — Huffman encoding procedures for DC/AC coefficients
 */
static VOID EncodeMCU(EncoderState *state, float *mcu, float *qt,
					  UINT8 *huffDcLen, UINT16 *huffDcCode,
					  UINT8 *huffAcLen, UINT16 *huffAcCode,
					  const UINT8 *zigZag, const EncodeConstants *c,
					  INT32 *pred, UINT64 *bitbuffer, UINT32 *location)
{
	INT32 du[64];

	// In place: the caller refills the sample buffer for the next block
	ForwardDCT(mcu, c);

	// Quantize with a single conversion per coefficient: the biased int minus
	// 1024 is already the rounded value — the old int->float->int round-trip
	// rebuilt the integer it already had
	float bias = c->quantBias;
	float half = c->quantHalf;
	for (INT32 i = 0; i < 64; ++i)
	{
		INT32 ival = (INT32)(mcu[i] * qt[i] + bias + half);
		du[zigZag[i]] = ival - 1024;
	}

	EncodeEntropy(state, du, huffDcLen, huffDcCode, huffAcLen, huffAcCode,
				  pred, bitbuffer, location);
}

/**
 * @brief Entropy-code one quantized 8x8 block (DC differential + AC RLE)
 *
 * @details The coefficient stage of the encoder shared by the scalar path
 * (EncodeMCU) and the SIMD path (EncodeBlockDu). du is in zig-zag order.
 *
 * @param state Encoder state
 * @param du 64 quantized coefficients in zig-zag order
 * @param huffDcLen DC Huffman code sizes
 * @param huffDcCode DC Huffman code values
 * @param huffAcLen AC Huffman code sizes
 * @param huffAcCode AC Huffman code values
 * @param pred Previous DC coefficient (updated on return)
 * @param bitbuffer Bit accumulator (updated on return)
 * @param location Bit position in accumulator (updated on return)
 *
 * @see ITU-T T.81 F.1.2 — Huffman encoding procedures for DC/AC coefficients
 */
static VOID EncodeEntropy(EncoderState *state, const INT32 *du,
						  UINT8 *huffDcLen, UINT16 *huffDcCode,
						  UINT8 *huffAcLen, UINT16 *huffAcCode,
						  INT32 *pred, UINT64 *bitbuffer, UINT32 *location)
{
	UINT16 vli[2];

	// DC coefficient (differential encoding)
	INT32 diff = du[0] - *pred;
	*pred = du[0];
	if (diff != 0)
	{
		CalculateVLI(diff, vli);
		// One merged emit: the code bits and the VLI bits are consecutive
		// in the stream, so (code << cat) | vli under a single call
		// produces exactly the same bits as the two-call form
		UINT32 cat = vli[1];
		WriteBits(state, bitbuffer, location, (UINT16)(huffDcLen[cat] + cat),
				  (UINT32)huffDcCode[cat] << cat | vli[0]);
	}
	else
	{
		WriteBits(state, bitbuffer, location, huffDcLen[0], huffDcCode[0]);
	}

	// AC coefficients (run-length encoding)
	INT32 lastNonZero = 0;
	for (INT32 i = 63; i > 0; --i)
	{
		if (du[i] != 0)
		{
			lastNonZero = i;
			break;
		}
	}

	for (INT32 i = 1; i <= lastNonZero; ++i)
	{
		INT32 zeroCount = 0;
		while (du[i] == 0)
		{
			++zeroCount;
			++i;
			if (zeroCount == 16)
			{
				// ZRL: 16 consecutive zeros
				WriteBits(state, bitbuffer, location, huffAcLen[0xF0], huffAcCode[0xF0]);
				zeroCount = 0;
			}
		}
		CalculateVLI(du[i], vli);

		UINT16 sym = (UINT16)((UINT16)zeroCount << 4) | vli[1];
		UINT32 cat = vli[1];
		WriteBits(state, bitbuffer, location, (UINT16)(huffAcLen[sym] + cat),
				  (UINT32)huffAcCode[sym] << cat | vli[0]);
	}

	if (lastNonZero != 63)
	{
		// EOB marker
		WriteBits(state, bitbuffer, location, huffAcLen[0], huffAcCode[0]);
	}
}

/**
 * @brief Encode one 8x8 block with the given component class's Huffman tables
 * @param luma True for luma (QT 0, HT 0/1), false for chroma (QT 1, HT 2/3)
 */
static VOID EncodeBlock(EncoderState *state, float *block, float *qt, BOOL luma,
						const UINT8 *zigZag, const EncodeConstants *c,
						INT32 *pred, UINT64 *bitbuffer, UINT32 *location)
{
	UINT32 dc = luma ? LumaDC : ChromaDC;
	UINT32 ac = luma ? LumaAC : ChromaAC;
	EncodeMCU(state, block, qt,
			  state->ehuffsize[dc], state->ehuffcode[dc],
			  state->ehuffsize[ac], state->ehuffcode[ac],
			  zigZag, c, pred, bitbuffer, location);
}

// ============================================================
//  Sample loading (RGB → level-shifted YCbCr)
// ============================================================

/**
 * @brief Convert one RGB pixel to luma for the 4:2:0 path
 *
 * @details Keeps the sub-LSB precision of the float path via one scaled
 * multiply over the 16.16 fixed-point accumulator (integer rounding of luma
 * measurably inflates q75 output on luma-dense content).
 */
static VOID ConvertLuma420(UINT8 r, UINT8 g, UINT8 b, float *y, const EncodeConstants *c)
{
	*y = (float)(19595 * (INT32)r + 38470 * (INT32)g + 7471 * (INT32)b) * c->lumaScale + c->neg128;
}

/**
 * @brief Convert one RGB pixel to the chroma pair for the 4:2:0 path
 * @details 16.16 fixed-point BT.601; integer rounding is immaterial under 2x2 subsampling
 */
static VOID ConvertChroma420(UINT8 r, UINT8 g, UINT8 b, INT32 *cb, INT32 *cr)
{
	*cb = (-11059 * (INT32)r - 21712 * (INT32)g + 32768 * (INT32)b + 32768) >> 16;
	*cr = (32768 * (INT32)r - 27441 * (INT32)g - 5328 * (INT32)b + 32768) >> 16;
}

/**
 * @brief Load one 8x8 block of all three components (4:4:4 path)
 *
 * @details Keeps the original float RGB-to-YCbCr conversion so quality >= 90
 * produces the same bitstream as the pre-subsampling encoder (the only
 * difference anywhere is the removed empty 4-byte COM segment).
 */
static VOID LoadFullBlock(const UINT8 *srcData, INT32 width, INT32 height, INT32 stride,
						  INT32 srcNumComponents, INT32 blockX, INT32 blockY,
						  float *duY, float *duCb, float *duCr, const EncodeConstants *c)
{
	// RGB-to-YCbCr conversion constants
	float kR = c->yccR;
	float kG = c->yccG;
	float kB = c->yccB;
	float kCbR = c->yccCbR;
	float kCbG = c->yccCbG;
	float kCbB = c->yccCbB;
	float kCrR = c->yccCrR;
	float kCrG = c->yccCrG;
	float kCrB = c->yccCrB;
	float neg128 = c->neg128;

	for (INT32 offY = 0; offY < 8; ++offY)
	{
		INT32 row = blockY + offY;
		if (row >= height)
			row = height - 1;
		for (INT32 offX = 0; offX < 8; ++offX)
		{
			INT32 col = blockX + offX;
			if (col >= width)
				col = width - 1;
			const UINT8 *px = srcData + ((USIZE)row * (USIZE)stride + (USIZE)col) * (USIZE)srcNumComponents;
			UINT8 b = px[2];
			UINT8 g = px[1];
			UINT8 r = px[0];

			float rf = (float)(INT32)r;
			float gf = (float)(INT32)g;
			float bf = (float)(INT32)b;

			INT32 blockIndex = offY * 8 + offX;
			duY[blockIndex] = kR * rf + kG * gf + kB * bf + neg128;
			duCb[blockIndex] = kCbR * rf - kCbG * gf + kCbB * bf;
			duCr[blockIndex] = kCrR * rf - kCrG * gf - kCrB * bf;
		}
	}
}

/**
 * @brief Load one 16x16 MCU: four luma blocks plus the 2x2-averaged chroma pair
 *
 * @details A single pass over the MCU's source pixels: each pixel feeds its
 * luma block and accumulates into the chroma sample covering it, so the
 * source is visited exactly once. luma[q] follows the T.81 A.2.3 quadrant
 * order Y1 (top-left), Y2 (top-right), Y3 (bottom-left), Y4 (bottom-right).
 * Edge pixels replicate into partial MCUs via row/col clamping.
 */
static VOID LoadMcu(const UINT8 *srcData, INT32 width, INT32 height, INT32 stride,
					INT32 srcNumComponents, INT32 mcuX, INT32 mcuY,
					float *luma /* [4][64] */, float *duCb, float *duCr,
					const EncodeConstants *c)
{
	UINT32 rAcc[64];
	UINT32 gAcc[64];
	UINT32 bAcc[64];
	Memory::Zero(rAcc, sizeof(rAcc));
	Memory::Zero(gAcc, sizeof(gAcc));
	Memory::Zero(bAcc, sizeof(bAcc));

	for (INT32 offY = 0; offY < 16; ++offY)
	{
		INT32 row = mcuY + offY;
		if (row >= height)
			row = height - 1;
		for (INT32 offX = 0; offX < 16; ++offX)
		{
			INT32 col = mcuX + offX;
			if (col >= width)
				col = width - 1;
			const UINT8 *px = srcData + ((USIZE)row * (USIZE)stride + (USIZE)col) * (USIZE)srcNumComponents;

			UINT32 accIdx = (USIZE)(offY >> 1) * 8 + (UINT32)(offX >> 1);
			rAcc[accIdx] += px[0];
			gAcc[accIdx] += px[1];
			bAcc[accIdx] += px[2];

			UINT32 quadrant = (UINT32)((offY >= 8) ? 2 : 0) + (UINT32)((offX >= 8) ? 1 : 0);
			UINT32 blockIndex = (UINT32)((offY & 7) * 8 + (offX & 7));
			ConvertLuma420(px[0], px[1], px[2], &luma[quadrant * 64 + blockIndex], c);
		}
	}

	// Finish the chroma pair from the accumulated 2x2 averages
	for (UINT32 i = 0; i < 64; ++i)
	{
		UINT8 r = (UINT8)((rAcc[i] + 2) >> 2);
		UINT8 g = (UINT8)((gAcc[i] + 2) >> 2);
		UINT8 b = (UINT8)((bAcc[i] + 2) >> 2);
		INT32 cb, cr;
		ConvertChroma420(r, g, b, &cb, &cr);
		duCb[i] = (float)cb;
		duCr[i] = (float)cr;
	}
}

#if defined(ARCHITECTURE_X86_64)

/**
 * @brief Entropy-code one already-quantized block (the SIMD path's entry)
 * @param du 64 quantized coefficients in zig-zag order
 * @param luma True for luma (HT 0/1), false for chroma (HT 2/3)
 */
static VOID EncodeBlockDu(EncoderState *state, const INT32 *du, BOOL luma,
						  INT32 *pred, UINT64 *bitbuffer, UINT32 *location)
{
	UINT32 dc = luma ? LumaDC : ChromaDC;
	UINT32 ac = luma ? LumaAC : ChromaAC;
	EncodeEntropy(state, du,
				  state->ehuffsize[dc], state->ehuffcode[dc],
				  state->ehuffsize[ac], state->ehuffcode[ac],
				  pred, bitbuffer, location);
}

/**
 * @brief Load one 16x16 MCU through the SSE2 path
 *
 * @details Requires the caller to guarantee mcuX + 16 <= width (every
 * column real, so the three 16-byte row loads stay inside the row and the
 * buffer). Row clamping below the MCU re-reads the last source row, which
 * matches the scalar edge replication exactly.
 *
 * Output layout is built for the 4-wide DCT: luma4[i] lane q = sample i of
 * quadrant block q (Y1 top-left, Y2 top-right, Y3 bottom-left, Y4
 * bottom-right), and chroma4[i] = [Cb_i, Cr_i, 0, 0].
 *
 * The math matches the scalar LoadMcu exactly: the luma fixed-point
 * accumulator is the same 19595R + 38470G + 7471B integer (the 0.587G term
 * split across the two pmaddwd weight pairs), and the chroma pair goes
 * through the same averaged fixed-point conversion. The -128 level shift
 * and the 2^-16 scale ride the DCT's DC correction and the quantize
 * factors respectively, keeping the per-row loop free of both.
 *
 * @param srcData Raw pixel data
 * @param height Image height for row clamping
 * @param stride Row pitch in pixels
 * @param srcNumComponents Bytes per pixel (3 = RGB, 4 = RGBA)
 * @param mcuX Left column of the MCU (mcuX + 16 <= width)
 * @param mcuY Top row of the MCU
 * @param luma4 Output: 64 quadrant-interleaved luma sample vectors
 * @param chroma4 Output: 64 [Cb, Cr, 0, 0] chroma sample vectors
 * @param k Per-encode SSE2 constants
 */
static VOID LoadMcuSse(const UINT8 *srcData, INT32 height, INT32 stride,
					   INT32 srcNumComponents, INT32 mcuX, INT32 mcuY,
					   V4F *luma4, V4F *chroma4, const Sse2Const *k)
{
	V8S accR[8], accG[8], accB[8];
	{
		V8S z = VecCast<V8S>(VSplat(0));
		for (INT32 i = 0; i < 8; ++i)
		{
			accR[i] = z;
			accG[i] = z;
			accB[i] = z;
		}
	}

	// Row pairs (r, r+8) put all four quadrant samples of luma4 index
	// (r, c) in flight together for the 4x4 transposes below
	for (INT32 r = 0; r < 8; ++r)
	{
		V4S eTop[2], oTop[2], eBot[2], oBot[2];
		for (INT32 half = 0; half < 2; ++half)
		{
			INT32 offY = r + half * 8;
			INT32 row = mcuY + offY;
			if (row >= height)
				row = height - 1;
			const UINT8 *p = srcData + ((USIZE)row * (USIZE)stride + (USIZE)mcuX) * (USIZE)srcNumComponents;

			RowPlanes pl;
			if (srcNumComponents == 3)
				DeinterleaveRgb3(p, &pl);
			else
				DeinterleaveRgba4(p, &pl);

			// Chroma 2x2 box accumulation: even + odd is the horizontal pair
			accR[offY >> 1] += pl.re + pl.ro;
			accG[offY >> 1] += pl.ge + pl.go;
			accB[offY >> 1] += pl.be + pl.bo;

			// Luma fixed point via the (R,G)/(B,G) weight-pair split; no
			// -128 fold (a constant image only shifts DC, corrected in the DCT)
			V4S *ev = half ? eBot : eTop;
			V4S *od = half ? oBot : oTop;
			ev[0] = PmaddWD(PunpckLWD(pl.re, pl.ge), k->yRG) +
					PmaddWD(PunpckLWD(pl.be, pl.ge), k->yBG);
			ev[1] = PmaddWD(PunpckHWD(pl.re, pl.ge), k->yRG) +
					PmaddWD(PunpckHWD(pl.be, pl.ge), k->yBG);
			od[0] = PmaddWD(PunpckLWD(pl.ro, pl.go), k->yRG) +
					PmaddWD(PunpckLWD(pl.bo, pl.go), k->yBG);
			od[1] = PmaddWD(PunpckHWD(pl.ro, pl.go), k->yRG) +
					PmaddWD(PunpckHWD(pl.bo, pl.go), k->yBG);
		}

		// To float unscaled: the DCT is linear, so the 2^-16 fixed-point
		// scale rides the quantize factors instead (pqtLuma4 is pre-scaled)
		// and the -128 shift lands in the DC correction
		V4F eF[4], oF[4];
		{
			V4S eS[4] = {eTop[0], eTop[1], eBot[0], eBot[1]};
			V4S oS[4] = {oTop[0], oTop[1], oBot[0], oBot[1]};
			for (INT32 i = 0; i < 4; ++i)
			{
				eF[i] = Cvtdq2ps(eS[i]);
				oF[i] = Cvtdq2ps(oS[i]);
			}
		}

		// Quadrant interleave: 4x4 transposes over (top-left, top-right,
		// bottom-left, bottom-right); columns land on luma4 row indices,
		// even-index columns from the even-pixel transpose, odd from odd
		V4S e0 = VecCast<V4S>(eF[0]), e1 = VecCast<V4S>(eF[1]);
		V4S e2 = VecCast<V4S>(eF[2]), e3 = VecCast<V4S>(eF[3]);
		V4S o0 = VecCast<V4S>(oF[0]), o1 = VecCast<V4S>(oF[1]);
		V4S o2 = VecCast<V4S>(oF[2]), o3 = VecCast<V4S>(oF[3]);
		V4S el01 = PunpckLDQ(e0, e1), eh01 = PunpckHDQ(e0, e1);
		V4S el23 = PunpckLDQ(e2, e3), eh23 = PunpckHDQ(e2, e3);
		V4S ol01 = PunpckLDQ(o0, o1), oh01 = PunpckHDQ(o0, o1);
		V4S ol23 = PunpckLDQ(o2, o3), oh23 = PunpckHDQ(o2, o3);
		luma4[r * 8 + 0] = VecCast<V4F>(PunpckLQDQ(el01, el23));
		luma4[r * 8 + 2] = VecCast<V4F>(PunpckHQDQ(el01, el23));
		luma4[r * 8 + 4] = VecCast<V4F>(PunpckLQDQ(eh01, eh23));
		luma4[r * 8 + 6] = VecCast<V4F>(PunpckHQDQ(eh01, eh23));
		luma4[r * 8 + 1] = VecCast<V4F>(PunpckLQDQ(ol01, ol23));
		luma4[r * 8 + 3] = VecCast<V4F>(PunpckHQDQ(ol01, ol23));
		luma4[r * 8 + 5] = VecCast<V4F>(PunpckLQDQ(oh01, oh23));
		luma4[r * 8 + 7] = VecCast<V4F>(PunpckHQDQ(oh01, oh23));
	}

	// Chroma finish: 2x2 average, fixed-point conversion, [Cb,Cr,0,0] lanes
	for (INT32 row = 0; row < 8; ++row)
	{
		V8S r = Psrlw(accR[row] + k->chromaHalf, 2);
		V8S g = Psrlw(accG[row] + k->chromaHalf, 2);
		V8S b = Psrlw(accB[row] + k->chromaHalf, 2);
		V8S zero = r ^ r;

		// (0, x) unpacked puts x in the high half of a 32-bit lane, so a
		// single >> 1 yields x << 15 = 0.5 * 65536 * x
		V4S cbL = PmaddWD(PunpckLWD(r, g), k->cbRG) +
				  Psrld(PunpckLWD(zero, b), 1) + k->chromaBias;
		V4S cbH = PmaddWD(PunpckHWD(r, g), k->cbRG) +
				  Psrld(PunpckHWD(zero, b), 1) + k->chromaBias;
		V4S crL = PmaddWD(PunpckLWD(g, b), k->crGB) +
				  Psrld(PunpckLWD(zero, r), 1) + k->chromaBias;
		V4S crH = PmaddWD(PunpckHWD(g, b), k->crGB) +
				  Psrld(PunpckHWD(zero, r), 1) + k->chromaBias;

		V4S cbLv = Psrad(cbL, 16), cbHv = Psrad(cbH, 16);
		V4S crLv = Psrad(crL, 16), crHv = Psrad(crH, 16);

		V4S p01 = PunpckLDQ(cbLv, crLv);
		V4S p23 = PunpckHDQ(cbLv, crLv);
		V4S p45 = PunpckLDQ(cbHv, crHv);
		V4S p67 = PunpckHDQ(cbHv, crHv);
		V4S z = p01 ^ p01;
		chroma4[row * 8 + 0] = Cvtdq2ps(PunpckLQDQ(p01, z));
		chroma4[row * 8 + 1] = Cvtdq2ps(PunpckHQDQ(p01, z));
		chroma4[row * 8 + 2] = Cvtdq2ps(PunpckLQDQ(p23, z));
		chroma4[row * 8 + 3] = Cvtdq2ps(PunpckHQDQ(p23, z));
		chroma4[row * 8 + 4] = Cvtdq2ps(PunpckLQDQ(p45, z));
		chroma4[row * 8 + 5] = Cvtdq2ps(PunpckHQDQ(p45, z));
		chroma4[row * 8 + 6] = Cvtdq2ps(PunpckLQDQ(p67, z));
		chroma4[row * 8 + 7] = Cvtdq2ps(PunpckHQDQ(p67, z));
	}
}

/**
 * @brief Load, transform, quantize, and entropy-code one full 4:2:0 MCU (SSE2)
 *
 * @details The four luma blocks run through one 4-wide DCT/quantize pass
 * and the Cb/Cr pair through one half-utilized pass (lanes 2-3 are zeros),
 * then each block is zig-zag scattered and entropy-coded in the same Y1 Y2
 * Y3 Y4 Cb Cr order as the scalar path.
 *
 * @param state Encoder state
 * @param srcData Raw pixel data
 * @param height Image height for row clamping
 * @param stride Row pitch in pixels
 * @param srcNumComponents Bytes per pixel (3 = RGB, 4 = RGBA)
 * @param mcuX Left column of the MCU (mcuX + 16 <= width)
 * @param mcuY Top row of the MCU
 * @param sseConst Per-encode SSE2 constants
 * @param pqtLuma4 Luma quantize vectors, broadcast per coefficient
 * @param pqtChroma4 Chroma quantize vectors, broadcast per coefficient
 * @param zigZag Zig-zag reordering table
 * @param predY Luma DC predictor (updated on return)
 * @param predCb Cb DC predictor (updated on return)
 * @param predCr Cr DC predictor (updated on return)
 * @param bitbuffer Bit accumulator (updated on return)
 * @param location Bit position in accumulator (updated on return)
 */
static VOID EncodeMcuSse(EncoderState *state, const UINT8 *srcData, INT32 height, INT32 stride,
						 INT32 srcNumComponents, INT32 mcuX, INT32 mcuY,
						 const Sse2Const *sseConst, const V4F *pqtLuma4, const V4F *pqtChroma4,
						 const UINT8 *zigZag,
						 INT32 *predY, INT32 *predCb, INT32 *predCr,
						 UINT64 *bitbuffer, UINT32 *location)
{
	V4F luma4[64];
	V4F chroma4[64];
	V4S du4[64];
	INT32 duY[4][64];
	INT32 duC[2][64];

	LoadMcuSse(srcData, height, stride, srcNumComponents, mcuX, mcuY, luma4, chroma4, sseConst);

	// Y1 Y2 Y3 Y4: one 4-wide DCT+quantize + transpose covers the quadrants
	ForwardDCTQuantize4(luma4, pqtLuma4, du4, sseConst->dctDcBias, sseConst);
	Transpose4Zig(du4, duY[0], duY[1], duY[2], duY[3], 4, zigZag);
	for (UINT32 q = 0; q < 4; ++q)
		EncodeBlockDu(state, duY[q], true, predY, bitbuffer, location);

	// Cb Cr: lanes 0-1 carry the pair, lanes 2-3 are zeros; already centered
	ForwardDCTQuantize4(chroma4, pqtChroma4, du4, VecCast<V4F>(VSplat(0)), sseConst);
	Transpose4Zig(du4, duC[0], duC[1], duC[1], duC[1], 2, zigZag);
	EncodeBlockDu(state, duC[0], false, predCb, bitbuffer, location);
	EncodeBlockDu(state, duC[1], false, predCr, bitbuffer, location);
}

#endif // ARCHITECTURE_X86_64



// ============================================================
//  Main encoding loop
// ============================================================

/**
 * @brief Encode all MCU blocks and write compressed scan data
 *
 * @param state Encoder state (Huffman tables and QT must be initialized)
 * @param srcData Raw pixel data
 * @param width Image width
 * @param height Image height
 * @param srcNumComponents Bytes per pixel (3 or 4)
 * @param stride Row pitch in pixels (width for packed rows; the enclosing
 *        frame's width when encoding an in-place sub-rectangle)
 * @param subsampleChroma True encodes 4:2:0 (16x16 MCU, 2x2 box-filtered
 *        chroma); false keeps the original 4:4:4 8x8 block layout
 */
static VOID EncodeImageData(EncoderState *state, const UINT8 *srcData,
							INT32 width, INT32 height, INT32 srcNumComponents,
							INT32 stride, BOOL subsampleChroma)
{
	UINT8 zigZag[64];
	InitZigZag(zigZag);

	// Build scaled quantization matrices (1/divisor for multiplication)
	// scalefactor[0] = 1, scalefactor[k] = cos(k*PI/16) * sqrt(2) for k=1..7
	// Actual divisor = 8 * scalefactor[row] * scalefactor[col] * QT[zigzag[i]]
	float aanScales[8];
	aanScales[0] = F32(0x3F800000); // 1.0f
	aanScales[1] = F32(0x3FB18A86); // 1.387039845f
	aanScales[2] = F32(kDctC2P6Bits); // 1.306562965f
	aanScales[3] = F32(0x3F968317); // 1.175875602f
	aanScales[4] = F32(0x3F800000); // 1.0f
	aanScales[5] = F32(0x3F49234E); // 0.785694958f
	aanScales[6] = F32(kDctC2C6Bits); // 0.541196100f
	aanScales[7] = F32(0x3E8D42AF); // 0.275899379f

	// Hot-path constants, materialized once (see EncodeConstants)
	EncodeConstants c;
	c.dctC4 = F32(kDctC4Bits);	  // cos(4*pi/16) * sqrt(2)
	c.dctC6 = F32(kDctC6Bits);	  // cos(6*pi/16) * sqrt(2)
	c.dctC2C6 = F32(kDctC2C6Bits);  // cos(2*pi/16) - cos(6*pi/16)
	c.dctC2P6 = F32(kDctC2P6Bits);  // cos(2*pi/16) + cos(6*pi/16)
	c.quantBias = F32(0x44800000); // 1024.0f
	c.quantHalf = F32(0x3F000000); // 0.5f
	c.lumaScale = F32(0x37800000); // 2^-16
	c.neg128 = F32(0xC3000000);	  // -128.0f
	c.yccR = F32(0x3E991687);	  // 0.299f
	c.yccG = F32(0x3F1645A2);	  // 0.587f
	c.yccB = F32(0x3DE978D5);	  // 0.114f
	c.yccCbR = F32(0xBE2CBFB1);	  // -0.1687f
	c.yccCbG = F32(0x3EA9A027);	  // 0.3313f (negative in formula)
	c.yccCbB = F32(0x3F000000);	  // 0.5f
	c.yccCrR = F32(0x3F000000);	  // 0.5f
	c.yccCrG = F32(0x3ED65FD9);	  // 0.4187f (negative in formula)
	c.yccCrB = F32(0x3DA6809D);	  // 0.0813f (negative in formula)

	ProcessedQT pqt;
	float one = F32(0x3F800000);   // 1.0f
	float eight = F32(0x41000000); // 8.0f
	for (INT32 y = 0; y < 8; y++)
	{
		for (INT32 x = 0; x < 8; x++)
		{
			INT32 i = y * 8 + x;
			pqt.luma[i] = one / (eight * aanScales[x] * aanScales[y] * state->qtLuma[zigZag[i]]);
			pqt.chroma[i] = one / (eight * aanScales[x] * aanScales[y] * state->qtChroma[zigZag[i]]);
		}
	}

	// Write JFIF header
	{
		JFIFHeader header;
		header.SOI = ByteOrder::Swap16(0xFFD8);
		header.APP0 = ByteOrder::Swap16(0xFFE0);
		UINT16 jfifLen = sizeof(JFIFHeader) - 4; // exclude SOI & APP0 markers
		header.jfifLen = ByteOrder::Swap16(jfifLen);
		const CHAR jfifId[] = "JFIF\0";
		Memory::Copy(header.jfifId, (PCVOID)jfifId, sizeof(jfifId));
		header.version = ByteOrder::Swap16(0x0102);
		header.units = 0x01;						// dots-per-inch
		UINT16 density = ByteOrder::Swap16(0x0060); // 96 DPI
		header.xDensity = density;
		header.yDensity = density;
		header.xThumb = 0;
		header.yThumb = 0;
		WriteOutput(state, &header, sizeof(JFIFHeader));
	}

	// Write quantization tables
	WriteDQT(state, state->qtLuma, 0x00);
	WriteDQT(state, state->qtChroma, 0x01);

	// Write SOF0 frame header (luma sampling 0x22 selects 4:2:0 MCUs)
	{
		FrameHeader header;
		header.SOF = ByteOrder::Swap16(0xFFC0);
		header.len = ByteOrder::Swap16(8 + 3 * 3);
		header.precision = 8;
		header.width = ByteOrder::Swap16((UINT16)width);
		header.height = ByteOrder::Swap16((UINT16)height);
		header.numComponents = 3;
		UINT8 qtSelectors[3];
		qtSelectors[0] = 0x00; // Luma uses QT 0
		qtSelectors[1] = 0x01; // Chroma uses QT 1
		qtSelectors[2] = 0x01; // Chroma uses QT 1
		for (INT32 i = 0; i < 3; ++i)
		{
			header.componentSpec[i].componentId = (UINT8)(i + 1);
			header.componentSpec[i].samplingFactors = (i == 0 && subsampleChroma) ? 0x22 : 0x11;
			header.componentSpec[i].qt = qtSelectors[i];
		}
		WriteOutput(state, &header, sizeof(FrameHeader));
	}

	// Write Huffman tables
	WriteDHT(state, state->htBits[LumaDC], state->htVals[LumaDC], DC, 0);
	WriteDHT(state, state->htBits[LumaAC], state->htVals[LumaAC], AC, 0);
	WriteDHT(state, state->htBits[ChromaDC], state->htVals[ChromaDC], DC, 1);
	WriteDHT(state, state->htBits[ChromaAC], state->htVals[ChromaAC], AC, 1);

	// Write SOS scan header
	{
		ScanHeader header;
		header.SOS = ByteOrder::Swap16(0xFFDA);
		header.len = ByteOrder::Swap16((UINT16)(6 + sizeof(ScanComponentSpec) * 3));
		header.numComponents = 3;
		UINT8 htSelectors[3];
		htSelectors[0] = 0x00; // Luma: DC 0 / AC 0
		htSelectors[1] = 0x11; // Cb: DC 1 / AC 1
		htSelectors[2] = 0x11; // Cr: DC 1 / AC 1
		for (INT32 i = 0; i < 3; ++i)
		{
			header.componentSpec[i].componentId = (UINT8)(i + 1);
			header.componentSpec[i].dcAc = htSelectors[i];
		}
		header.first = 0;
		header.last = 63;
		header.ahAl = 0;
		WriteOutput(state, &header, sizeof(ScanHeader));
	}

	// Encode scan data
	float duY[64];
	float luma[4 * 64];
	float duCb[64];
	float duCr[64];

	INT32 predY = 0;
	INT32 predCb = 0;
	INT32 predCr = 0;

	UINT64 bitbuffer = 0;
	UINT32 bitLocation = 0;

#if defined(ARCHITECTURE_X86_64)
	// SIMD constants and per-coefficient quantize vectors, once per encode
	Sse2Const sseConst;
	InitSse2Const(&sseConst);
	V4F pqtLuma4[64];
	V4F pqtChroma4[64];
	for (INT32 i = 0; i < 64; ++i)
	{
		pqtLuma4[i] = VecCast<V4F>(VSplat(__builtin_bit_cast(UINT32, pqt.luma[i] * c.lumaScale)));
		pqtChroma4[i] = VecCast<V4F>(VSplat(__builtin_bit_cast(UINT32, pqt.chroma[i])));
	}
#endif

	if (subsampleChroma)
	{
		// 4:2:0: 16x16 MCUs — one pass loads four luma blocks plus the
		// averaged chroma pair (T.81 A.2.3 order Y1 Y2 Y3 Y4 Cb Cr)
		for (INT32 y = 0; y < height; y += 16)
		{
			for (INT32 x = 0; x < width; x += 16)
			{
#if defined(ARCHITECTURE_X86_64)
				// Full MCUs only: the SIMD loader reads 16 real columns per
				// row, so the rightmost partial-MCU column stays scalar
				if (x + 16 <= width)
				{
					EncodeMcuSse(state, srcData, height, stride, srcNumComponents, x, y,
								 &sseConst, pqtLuma4, pqtChroma4, zigZag,
								 &predY, &predCb, &predCr, &bitbuffer, &bitLocation);
					continue;
				}
#endif
				LoadMcu(srcData, width, height, stride, srcNumComponents, x, y, luma, duCb, duCr, &c);

				for (UINT32 q = 0; q < 4; ++q)
					EncodeBlock(state, luma + q * 64, pqt.luma, true, zigZag, &c, &predY, &bitbuffer, &bitLocation);
				EncodeBlock(state, duCb, pqt.chroma, false, zigZag, &c, &predCb, &bitbuffer, &bitLocation);
				EncodeBlock(state, duCr, pqt.chroma, false, zigZag, &c, &predCr, &bitbuffer, &bitLocation);
			}
		}
	}
	else
	{
		// 4:4:4: 8x8 MCUs, one block per component
		for (INT32 y = 0; y < height; y += 8)
		{
			for (INT32 x = 0; x < width; x += 8)
			{
				LoadFullBlock(srcData, width, height, stride, srcNumComponents, x, y, duY, duCb, duCr, &c);

				EncodeBlock(state, duY, pqt.luma, true, zigZag, &c, &predY, &bitbuffer, &bitLocation);
				EncodeBlock(state, duCb, pqt.chroma, false, zigZag, &c, &predCb, &bitbuffer, &bitLocation);
				EncodeBlock(state, duCr, pqt.chroma, false, zigZag, &c, &predCr, &bitbuffer, &bitLocation);
			}
		}
	}

	// Flush remaining bits (pad to byte boundary), then drain whole bytes
	UINT32 partial = bitLocation & 7;
	if (partial != 0)
		WriteBits(state, &bitbuffer, &bitLocation, (UINT16)(8 - partial), 0);
	FlushBitBuffer(state, &bitbuffer, &bitLocation);

	// Write EOI marker
	UINT16 eoi = ByteOrder::Swap16(0xFFD9);
	WriteOutput(state, &eoi, sizeof(UINT16));

	// Flush remaining output buffer
	if (state->outputBufferCount > 0)
	{
		state->writeContext.func(state->writeContext.context, state->outputBuffer, (INT32)state->outputBufferCount);
		state->outputBufferCount = 0;
	}
}

// ============================================================
//  Public API
// ============================================================

[[nodiscard]] Result<VOID, Error> JpegEncoder::Encode(
	JpegWriteFunc *func,
	PVOID context,
	INT32 quality,
	INT32 width,
	INT32 height,
	INT32 numComponents,
	Span<const UINT8> srcData)
{
	// Packed rows: the row pitch equals the image width
	return Encode(func, context, quality, width, height, numComponents, srcData, width);
}

[[nodiscard]] Result<VOID, Error> JpegEncoder::Encode(
	JpegWriteFunc *func,
	PVOID context,
	INT32 quality,
	INT32 width,
	INT32 height,
	INT32 numComponents,
	Span<const UINT8> srcData,
	INT32 stride)
{
	// Size math in UINT64: on 32-bit-size targets the USIZE product wraps and
	// would pass an undersized span
	UINT64 neededBytes = ((UINT64)stride * (UINT64)(height - 1) + (UINT64)width) * (UINT64)numComponents;
	if ((numComponents != 3 && numComponents != 4) || width <= 0 || height <= 0 ||
		width > 0xFFFF || height > 0xFFFF || stride < width ||
		neededBytes > (UINT64)srcData.Size())
	{
		return Result<VOID, Error>::Err(Error::Jpeg_InvalidParams);
	}

	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;

	// Standard luminance quantization table (ITU-T T.81 Annex K.1)
	UINT8 defaultQtLuma[64];
	{
		const UINT8 embedded[] = {
			16, 11, 10, 16, 24, 40, 51, 61,
			12, 12, 14, 19, 26, 58, 60, 55,
			14, 13, 16, 24, 40, 57, 69, 56,
			14, 17, 22, 29, 51, 87, 80, 62,
			18, 22, 37, 56, 68, 109, 103, 77,
			24, 35, 55, 64, 81, 104, 113, 92,
			49, 64, 78, 87, 103, 121, 120, 101,
			72, 92, 95, 98, 112, 100, 103, 99};
		Memory::Copy(defaultQtLuma, embedded, 64);
	}

	// Standard chrominance quantization table (ITU-T T.81 Annex K.1)
	UINT8 defaultQtChroma[64];
	{
		const UINT8 embedded[] = {
			16, 18, 24, 47, 99, 99, 99, 99,
			18, 21, 26, 66, 99, 99, 99, 99,
			24, 26, 56, 99, 99, 99, 99, 99,
			47, 66, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99};
		Memory::Copy(defaultQtChroma, embedded, 64);
	}

	EncoderState state;
	Memory::Zero(&state, sizeof(EncoderState));

	// Scale quality factor (IJG formula)
	UINT32 qtFactor = (quality < 50) ? (5000 / (UINT32)quality) : (UINT32)(200 - quality * 2);

	// Scale quantization tables
	for (USIZE i = 0; i < 64; i++)
	{
		INT32 temp = (INT32)((defaultQtLuma[i] * qtFactor + 50) / 100);
		if (temp <= 0)
			temp = 1;
		if (temp > 255)
			temp = 255;
		state.qtLuma[i] = (UINT8)temp;
	}
	for (USIZE i = 0; i < 64; i++)
	{
		INT32 temp = (INT32)((defaultQtChroma[i] * qtFactor + 50) / 100);
		if (temp <= 0)
			temp = 1;
		if (temp > 255)
			temp = 255;
		state.qtChroma[i] = (UINT8)temp;
	}

	state.writeContext.context = context;
	state.writeContext.func = func;

	// Standard Huffman tables (ITU-T T.81 Annex K.3)

	// Chrominance AC values (K.3.3.2, Table K.6)
	const UINT8 htChromaAcVals[] = {
		0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
		0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
		0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
		0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
		0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
		0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
		0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
		0xF9, 0xFA};

	// Standard Huffman BITS tables (ITU-T T.81 Annex K.3)

	// Luminance DC BITS (K.3.3.1, Table K.3)
	UINT8 lumaDcBits[16];
	{
		const UINT8 e[] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
		Memory::Copy(lumaDcBits, e, 16);
	}

	// Luminance AC BITS (K.3.3.2, Table K.5)
	UINT8 lumaAcBits[16];
	{
		const UINT8 e[] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D};
		Memory::Copy(lumaAcBits, e, 16);
	}

	// Chrominance DC BITS (K.3.3.1, Table K.4)
	UINT8 chromaDcBits[16];
	{
		const UINT8 e[] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
		Memory::Copy(chromaDcBits, e, 16);
	}

	// Chrominance AC BITS (K.3.3.2, Table K.6)
	UINT8 chromaAcBits[16];
	{
		const UINT8 e[] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
		Memory::Copy(chromaAcBits, e, 16);
	}

	state.htBits[LumaDC] = lumaDcBits;
	state.htBits[LumaAC] = lumaAcBits;
	state.htBits[ChromaDC] = chromaDcBits;
	state.htBits[ChromaAC] = chromaAcBits;

	// Standard Huffman VALS tables (ITU-T T.81 Annex K.3)

	// DC values (K.3.3.1, Tables K.3 & K.4)
	UINT8 dcVals[12];
	{
		const UINT8 e[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		Memory::Copy(dcVals, e, 12);
	}

	// Luminance AC values (K.3.3.2, Table K.5)
	UINT8 lumaAcVals[162];
	{
		const UINT8 e[] = {
			0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
			0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
			0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
			0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
			0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
			0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
			0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
			0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
			0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
			0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
			0xF9, 0xFA};
		Memory::Copy(lumaAcVals, e, 162);
	}

	// Chrominance AC values (K.3.3.2, Table K.6)
	UINT8 chromaAcVals[162];
	Memory::Copy(chromaAcVals, htChromaAcVals, 162);

	state.htVals[LumaDC] = dcVals;
	state.htVals[LumaAC] = lumaAcVals;
	state.htVals[ChromaDC] = dcVals;
	state.htVals[ChromaAC] = chromaAcVals;

	// Build extended Huffman tables
	INT32 tableLengths[4] = {0, 0, 0, 0};
	for (INT32 i = 0; i < 4; ++i)
		for (INT32 k = 0; k < 16; ++k)
			tableLengths[i] += state.htBits[i][k];

	UINT8 huffsize[4][257];
	UINT16 huffcode[4][256];
	for (INT32 i = 0; i < 4; ++i)
	{
		GenerateCodeLengths(huffsize[i], state.htBits[i]);
		GenerateCodes(huffcode[i], huffsize[i]);
	}
	for (INT32 i = 0; i < 4; ++i)
	{
		BuildExtendedTable(state.ehuffsize[i], state.ehuffcode[i],
						   state.htVals[i], huffsize[i], huffcode[i], tableLengths[i]);
	}

	// Quality gate: below 90, 4:2:0 chroma subsampling roughly halves the
	// DCT/entropy work and shrinks output 25-40% with little visual loss on
	// screen content; 90+ keeps the original 4:4:4 fidelity
	BOOL subsampleChroma = quality < 90;
	EncodeImageData(&state, srcData.Data(), width, height, numComponents, stride, subsampleChroma);

	return Result<VOID, Error>::Ok();
}
