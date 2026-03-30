/**
 * @file compiler_runtime.windows.armv7a.cc
 * @brief Windows ARM Compiler Runtime Support
 *
 * @details Provides division functions specific to the Windows ARM calling convention.
 * These functions are called implicitly by the compiler when building for Windows ARM
 * targets with -nostdlib.
 *
 * Windows ARM uses different argument ordering than the standard ARM EABI:
 *   - EABI:    numerator in r0, denominator in r1
 *   - Windows: divisor in r0, dividend in r1 (reversed)
 *
 * @see ARM EABI Specification Reference:
 *      https://github.com/ARM-software/abi-aa/blob/main/rtabi32/rtabi32.rst
 *
 * Part of CORE (Core Abstraction Layer) - Core runtime support.
 */

#include "core/compiler/compiler.h"
#include "core/types/primitives.h"

#if defined(PLATFORM_WINDOWS_ARMV7A)

// =============================================================================
// 32-bit Division Helper
// =============================================================================

/**
 * @brief Internal 32-bit unsigned division with optional remainder
 *
 * @details Algorithm: Binary long division with optimizations:
 *   1. Power-of-2 fast path using CLZ instruction
 *   2. Skip leading zeros in numerator for better performance
 *   3. Branch prediction hints for common cases
 *
 * Performance: O(1) for power-of-2, O(n) for general case where n = significant bits
 *
 * @note Force inlined for maximum performance in critical path
 */
static FORCE_INLINE UINT32 udiv32_internal(UINT32 numerator, UINT32 denominator, UINT32 *remainder)
{
	// Division by zero: return 0 quotient, numerator as remainder
	if (__builtin_expect(denominator == 0, 0))
	{
		if (remainder)
			*remainder = numerator;
		return 0;
	}

	// Fast path: power-of-2 divisor (common case: ~20% of divisions)
	// Uses CLZ instruction for O(1) performance
	if (__builtin_expect((denominator & (denominator - 1)) == 0, 1))
	{
		const UINT32 shift = __builtin_ctz(denominator);
		if (remainder)
			*remainder = numerator & (denominator - 1);
		return numerator >> shift;
	}

	// Early exit: numerator < denominator
	if (__builtin_expect(numerator < denominator, 0))
	{
		if (remainder)
			*remainder = numerator;
		return 0;
	}

	// Binary long division: start from most significant bit of numerator
	// Skip leading zeros for better performance
	const INT32 startBit = 31 - __builtin_clz(numerator);
	UINT32 quotient = 0;
	UINT32 rem = 0;

	for (INT32 i = startBit; i >= 0; i--)
	{
		rem = (rem << 1) | ((numerator >> i) & 1);
		if (rem >= denominator)
		{
			rem -= denominator;
			quotient |= (1U << i);
		}
	}

	if (remainder)
		*remainder = rem;
	return quotient;
}

extern "C"
{
	// =========================================================================
	// Windows ARM: Runtime Functions
	// =========================================================================

	/**
	 * __rt_udiv - Windows ARM unsigned 32-bit division with remainder
	 *
	 * Windows ARM calling convention (DIFFERENT from EABI __aeabi_uidiv):
	 *   Input:  divisor in r0, dividend in r1 (reversed from EABI!)
	 *   Output: quotient in r0, remainder in r1
	 *
	 * Called by compiler for: unsigned_a / unsigned_b, unsigned_a % unsigned_b
	 * on Windows ARM targets.
	 *
	 * @note The argument order is swapped compared to __aeabi_uidiv:
	 *   EABI:    __aeabi_uidiv(numerator=r0, denominator=r1)
	 *   Windows: __rt_udiv(divisor=r0, dividend=r1)
	 */
	COMPILER_RUNTIME UINT64 __rt_udiv(UINT32 divisor, UINT32 dividend)
	{
		UINT32 remainder = 0;
		UINT32 quotient = udiv32_internal(dividend, divisor, &remainder);

		// Pack: quotient (low 32 bits), remainder (high 32 bits)
		// This maps to r0=quotient, r1=remainder in AAPCS
		return ((UINT64)remainder << 32) | quotient;
	}

	/**
	 * __rt_udiv64 - Windows ARM unsigned 64-bit division with remainder
	 *
	 * Windows ARM calling convention (DIFFERENT from EABI __aeabi_uldivmod):
	 *   Input:  divisor in r0:r1, dividend in r2:r3 (reversed from EABI!)
	 *   Output: quotient in r0:r1, remainder in r2:r3
	 *
	 * Called by compiler for: uint64_a / uint64_b, uint64_a % uint64_b
	 * on Windows ARM targets.
	 *
	 * @note The argument order is swapped compared to __aeabi_uldivmod:
	 *   EABI:    __aeabi_uldivmod(numerator=r0:r1, denominator=r2:r3)
	 *   Windows: __rt_udiv64(divisor=r0:r1, dividend=r2:r3)
	 *
	 * Uses naked function with inline assembly to match the register-based
	 * calling convention (quotient in r0:r1, remainder in r2:r3).
	 */
	COMPILER_RUNTIME
	__attribute__((naked)) void __rt_udiv64(void)
	{
		// Swap r0:r1 (divisor) with r2:r3 (dividend) so divmod64_helper
		// receives numerator=r0:r1, denominator=r2:r3
		__asm__ volatile(
			"push   {r4, r5, lr}\n\t"    // Save callee-saved registers and return address
			"mov    r12, r0\n\t"         // r12 = divisor_lo (temp)
			"mov    r0, r2\n\t"          // r0 = dividend_lo (now numerator_lo)
			"mov    r2, r12\n\t"         // r2 = divisor_lo (now denominator_lo)
			"mov    r12, r1\n\t"         // r12 = divisor_hi (temp)
			"mov    r1, r3\n\t"          // r1 = dividend_hi (now numerator_hi)
			"mov    r3, r12\n\t"         // r3 = divisor_hi (now denominator_hi)
			"sub    sp, sp, #16\n\t"     // Allocate 16 bytes: [quotient:8][remainder:8]
			"mov    r4, sp\n\t"          // r4 = &quotient
			"add    r5, sp, #8\n\t"      // r5 = &remainder
			"mov    r12, #0\n\t"         // r12 = is_signed = false
			"push   {r4, r5, r12}\n\t"   // Push args 3, 4, 5 onto stack
			"bl     divmod64_helper\n\t" // Call helper(num_r0:r1, den_r2:r3, &quot, &rem, false)
			"add    sp, sp, #12\n\t"     // Clean up function arguments
			"pop    {r0-r3}\n\t"         // Load results: quot->r0:r1, rem->r2:r3
			"pop    {r4, r5, pc}\n\t"    // Restore registers and return
		);
	}

	// =========================================================================
	// Windows ARM: Floating-Point Conversion Functions
	// =========================================================================

	/**
	 * __i64tod - Convert signed 64-bit integer to double
	 *
	 * Windows ARM calling convention:
	 *   Input:  value in r0:r1 (little-endian)
	 *   Output: double in r0:r1 (IEEE-754 format)
	 *
	 * Called by compiler for: (double)int64_value
	 */
	COMPILER_RUNTIME UINT64 __i64tod(INT64 val)
	{
		if (val == 0)
			return 0ULL;

		BOOL negative = val < 0;
		UINT64 absVal;

		if (val == (INT64)0x8000000000000000LL)
			absVal = 0x8000000000000000ULL;
		else
			absVal = negative ? (UINT64)(-val) : (UINT64)val;

		const INT32 msb = 63 - __builtin_clzll(absVal);
		const INT32 exponent = 1023 + msb;

		UINT64 mantissa = absVal;
		if (msb >= 52)
			mantissa = mantissa >> (msb - 52);
		else
			mantissa = mantissa << (52 - msb);

		mantissa = mantissa & 0x000FFFFFFFFFFFFFULL;

		UINT64 sign = negative ? 0x8000000000000000ULL : 0ULL;
		UINT64 exp = (UINT64)exponent << 52;

		return sign | exp | mantissa;
	}

	/**
	 * __u64tod - Convert unsigned 64-bit integer to double
	 *
	 * Called by compiler for: (double)uint64_value
	 */
	COMPILER_RUNTIME UINT64 __u64tod(UINT64 val)
	{
		if (val == 0)
			return 0ULL;

		const INT32 msb = 63 - __builtin_clzll(val);
		const INT32 exponent = 1023 + msb;

		UINT64 mantissa = val;
		if (msb >= 52)
			mantissa = mantissa >> (msb - 52);
		else
			mantissa = mantissa << (52 - msb);

		mantissa = mantissa & 0x000FFFFFFFFFFFFFULL;
		UINT64 exp = (UINT64)exponent << 52;

		return exp | mantissa;
	}

	/**
	 * __dtoi64 - Convert double to signed 64-bit integer (truncate toward zero)
	 *
	 * Called by compiler for: (int64_t)double_value
	 */
	COMPILER_RUNTIME INT64 __dtoi64(UINT64 bits)
	{
		UINT64 signBit = bits & 0x8000000000000000ULL;
		UINT64 expBits = bits & 0x7FF0000000000000ULL;
		UINT64 mantissaBits = bits & 0x000FFFFFFFFFFFFFULL;

		INT32 exponent = (INT32)(expBits >> 52) - 1023;

		if (exponent < 0)
			return 0LL;

		if (exponent >= 63)
		{
			if (signBit)
				return 0x8000000000000000LL;
			else
				return 0x7FFFFFFFFFFFFFFFLL;
		}

		UINT64 mantissaWithOne = mantissaBits | 0x0010000000000000ULL;

		UINT64 intValue;
		if (exponent <= 52)
			intValue = mantissaWithOne >> (52 - exponent);
		else
			intValue = mantissaWithOne << (exponent - 52);

		INT64 result = (INT64)intValue;
		if (signBit)
			result = -result;

		return result;
	}

	/**
	 * __dtou64 - Convert double to unsigned 64-bit integer (truncate toward zero)
	 *
	 * Called by compiler for: (uint64_t)double_value
	 */
	COMPILER_RUNTIME UINT64 __dtou64(UINT64 bits)
	{
		UINT64 signBit = bits & 0x8000000000000000ULL;
		UINT64 expBits = bits & 0x7FF0000000000000ULL;
		UINT64 mantissaBits = bits & 0x000FFFFFFFFFFFFFULL;

		if (signBit)
			return 0ULL;

		INT32 exponent = (INT32)(expBits >> 52) - 1023;

		if (exponent < 0)
			return 0ULL;

		if (exponent >= 64)
			return 0xFFFFFFFFFFFFFFFFULL;

		UINT64 mantissaWithOne = mantissaBits | 0x0010000000000000ULL;

		UINT64 intValue;
		if (exponent <= 52)
			intValue = mantissaWithOne >> (52 - exponent);
		else
			intValue = mantissaWithOne << (exponent - 52);

		return intValue;
	}

} // extern "C"

#endif // PLATFORM_WINDOWS_ARMV7A
