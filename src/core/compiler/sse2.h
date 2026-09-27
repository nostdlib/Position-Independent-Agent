/**
 * @file sse2.h
 * @brief Minimal SSE2 Surface via Compiler Builtins
 *
 * @details The codebase includes only its own headers — no compiler or
 * library headers anywhere — so the vector type and the handful of SSE2
 * operations the hot paths use are declared here directly. The type is the
 * compiler's 16-byte integer vector (16 byte lanes); operators (^ | ==)
 * map to SSE2 instructions directly and the all-zero test uses
 * __builtin_reduce_or, which needs no declaration and no include. SSE2 is
 * baseline on x86_64, so no runtime detection.
 */

#pragma once

#include "core/compiler/compiler.h"

#if defined(ARCHITECTURE_X86_64)

/// @brief 16-lane byte vector — the compiler's 128-bit integer vector type
typedef signed char SseVec16b __attribute__((__vector_size__(16)));

/// @brief The same vector type with byte alignment — the unaligned-load view
typedef signed char SseVec16u __attribute__((__vector_size__(16), __aligned__(1)));

/// @brief Unaligned 16-byte load (movdqu)
/// @param p Source address, any alignment
/// @return The 16 bytes at p as a vector
static FORCE_INLINE SseVec16b SseLoadU(const VOID *p)
{
	return *(const SseVec16u *)p;
}

/// @brief Test a vector for all-zero lanes
/// @param v Vector to test
/// @return true when every byte of v is zero
static FORCE_INLINE BOOL SseAllZero(SseVec16b v)
{
	return __builtin_reduce_or(v) == 0;
}

#endif // ARCHITECTURE_X86_64
