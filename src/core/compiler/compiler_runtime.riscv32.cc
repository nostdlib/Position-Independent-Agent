/**
 * @file compiler_runtime.riscv32.cc
 * @brief RISC-V 32-bit Compiler Runtime Support
 *
 * @details Provides 64-bit division, modulo, and shift operations for 32-bit RISC-V
 * architecture. These functions are called implicitly by the compiler when building
 * with -nostdlib, as RV32 cannot perform 64-bit division/modulo in hardware.
 *
 * Performance characteristics:
 *   - Division by power-of-2: O(1) using __builtin_ctzll
 *   - General division: O(n) where n is bit width (64 bits)
 *   - Optimized for common cases (zero, power-of-2, small divisors)
 *
 * @see GCC libgcc Integer Library Routines
 *      https://gcc.gnu.org/onlinedocs/gccint/Integer-library-routines.html
 *
 * Part of CORE (Core Abstraction Layer) - Core runtime support.
 */

#include "core/compiler/compiler.h"
#include "core/compiler/compiler_builtins.h"
#include "core/types/primitives.h"

#if defined(ARCHITECTURE_RISCV32)

// =============================================================================
// 64-bit Division Helpers
// =============================================================================

/**
 * @brief Internal 64-bit unsigned division with quotient and remainder
 *
 * @details Algorithm: Binary long division with optimizations:
 *   1. Power-of-2 fast path using __builtin_ctzll
 *   2. Skip leading zeros in numerator for better performance
 *   3. Branch prediction hints for common cases
 *   4. Early exit for numerator < denominator
 *
 * Performance: O(1) for power-of-2, O(n) for general case where n = significant bits
 *
 * @note Force inlined for maximum performance in critical path
 */
static FORCE_INLINE VOID udiv64_internal(UINT64 numerator, UINT64 denominator,
										 UINT64 *quotient, UINT64 *remainder)
{
	// Division by zero: return 0 quotient, numerator as remainder
	if (__builtin_expect(denominator == 0, 0))
	{
		*quotient = 0;
		*remainder = numerator;
		return;
	}

	// Fast path: power-of-2 divisor (common case: ~20% of divisions)
	if (__builtin_expect((denominator & (denominator - 1ULL)) == 0, 1))
	{
		const UINT32 shift = __builtin_ctzll(denominator);
		*quotient = numerator >> shift;
		*remainder = numerator & (denominator - 1ULL);
		return;
	}

	// Early exit: numerator < denominator
	if (__builtin_expect(numerator < denominator, 0))
	{
		*quotient = 0;
		*remainder = numerator;
		return;
	}

	// Binary long division: start from most significant bit of numerator
	// Skip leading zeros for better performance
	const INT32 startBit = 63 - __builtin_clzll(numerator);
	UINT64 q = 0;
	UINT64 r = 0;

	for (INT32 i = startBit; i >= 0; i--)
	{
		r <<= 1;
		if ((numerator >> i) & 1ULL)
			r |= 1ULL;

		if (r >= denominator)
		{
			r -= denominator;
			q |= (1ULL << i);
		}
	}

	*quotient = q;
	*remainder = r;
}

extern "C"
{
	// =========================================================================
	// RISC-V 32: 64-bit Unsigned Division Functions
	// =========================================================================

	/**
	 * __udivdi3 - Unsigned 64-bit division
	 *
	 * GCC/Clang calling convention:
	 *   Input:  numerator (64-bit), denominator (64-bit)
	 *   Output: quotient (64-bit)
	 *
	 * Called by compiler for: uint64_t_a / uint64_t_b (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME UINT64 __udivdi3(UINT64 numerator, UINT64 denominator)
	{
		UINT64 quotient, remainder;
		udiv64_internal(numerator, denominator, &quotient, &remainder);
		return quotient;
	}

	/**
	 * __umoddi3 - Unsigned 64-bit modulo
	 *
	 * GCC/Clang calling convention:
	 *   Input:  numerator (64-bit), denominator (64-bit)
	 *   Output: remainder (64-bit)
	 *
	 * Called by compiler for: uint64_t_a % uint64_t_b (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME UINT64 __umoddi3(UINT64 numerator, UINT64 denominator)
	{
		UINT64 quotient, remainder;
		udiv64_internal(numerator, denominator, &quotient, &remainder);
		return remainder;
	}

	// =========================================================================
	// RISC-V 32: 64-bit Signed Division Helper
	// =========================================================================

	/**
	 * @brief Unified signed 64-bit division helper
	 *
	 * @details Handles both division and modulo operations with sign handling.
	 *
	 * Sign rules (per C standard):
	 *   - Quotient is negative if operands have opposite signs
	 *   - Remainder always takes the sign of the numerator
	 */
	static FORCE_INLINE VOID idiv64_internal(INT64 numerator, INT64 denominator,
											 INT64 *quotient, INT64 *remainder)
	{
		if (__builtin_expect(denominator == 0, 0))
		{
			*quotient = 0;
			*remainder = numerator;
			return;
		}

		// Determine result signs and convert to absolute values
		// Cast to UINT64 before negating to avoid signed overflow UB on INT64_MIN
		const BOOL negNum = numerator < 0;
		const BOOL negQuot = negNum != (denominator < 0);
		const UINT64 absNum = negNum ? -(UINT64)numerator : (UINT64)numerator;
		const UINT64 absDen = (denominator < 0) ? -(UINT64)denominator : (UINT64)denominator;

		// Perform unsigned division on absolute values
		UINT64 q, r;
		udiv64_internal(absNum, absDen, &q, &r);

		// Apply signs (remainder takes sign of numerator)
		*quotient = negQuot ? -(INT64)q : (INT64)q;
		*remainder = negNum ? -(INT64)r : (INT64)r;
	}

	// =========================================================================
	// RISC-V 32: 64-bit Signed Division Functions
	// =========================================================================

	/**
	 * __divdi3 - Signed 64-bit division
	 *
	 * Called by compiler for: int64_t_a / int64_t_b (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME INT64 __divdi3(INT64 numerator, INT64 denominator)
	{
		INT64 quotient, remainder;
		idiv64_internal(numerator, denominator, &quotient, &remainder);
		return quotient;
	}

	/**
	 * __moddi3 - Signed 64-bit modulo
	 *
	 * Called by compiler for: int64_t_a % int64_t_b (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME INT64 __moddi3(INT64 numerator, INT64 denominator)
	{
		INT64 quotient, remainder;
		idiv64_internal(numerator, denominator, &quotient, &remainder);
		return remainder;
	}

	// =========================================================================
	// RISC-V 32: 64-bit Shift Functions
	// =========================================================================

	/**
	 * __lshrdi3 - 64-bit logical shift right
	 *
	 * Called by compiler for: uint64_t_a >> shift_amount (on 32-bit RISC-V)
	 *
	 * Behavior: Shift amounts >= 64 or < 0 return 0.
	 */
	COMPILER_RUNTIME UINT64 __lshrdi3(UINT64 value, INT32 shift)
	{
		if (__builtin_expect((UINT32)shift >= 64, 0))
			return 0;
		return value >> shift;
	}

	/**
	 * __ashldi3 - 64-bit arithmetic shift left
	 *
	 * Called by compiler for: int64_t_a << shift_amount (on 32-bit RISC-V)
	 *
	 * Behavior: Shift amounts >= 64 or < 0 return 0.
	 */
	COMPILER_RUNTIME INT64 __ashldi3(INT64 value, INT32 shift)
	{
		if (__builtin_expect((UINT32)shift >= 64, 0))
			return 0;
		return value << shift;
	}

	// =========================================================================
	// RISC-V 32: Floating-Point Conversion Functions
	// =========================================================================

	/**
	 * __floatdidf - Convert signed 64-bit integer to double
	 *
	 * GCC libgcc calling convention:
	 *   Input:  value (64-bit signed integer, in a0+a1)
	 *   Output: double (in fa0 on ilp32d, or a0+a1 on ilp32)
	 *
	 * Called by compiler for: (double)int64_value (on 32-bit RISC-V)
	 *
	 * Algorithm: Manual IEEE-754 double construction (no FPU required)
	 */
	COMPILER_RUNTIME double __floatdidf(INT64 val)
	{
		if (val == 0)
			return __builtin_bit_cast(double, (UINT64)0ULL);

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

		return __builtin_bit_cast(double, sign | exp | mantissa);
	}

	/**
	 * __floatundidf - Convert unsigned 64-bit integer to double
	 *
	 * Called by compiler for: (double)uint64_value (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME double __floatundidf(UINT64 val)
	{
		if (val == 0)
			return __builtin_bit_cast(double, (UINT64)0ULL);

		const INT32 msb = 63 - __builtin_clzll(val);
		const INT32 exponent = 1023 + msb;

		UINT64 mantissa = val;
		if (msb >= 52)
			mantissa = mantissa >> (msb - 52);
		else
			mantissa = mantissa << (52 - msb);

		mantissa = mantissa & 0x000FFFFFFFFFFFFFULL;
		UINT64 exp = (UINT64)exponent << 52;

		return __builtin_bit_cast(double, exp | mantissa);
	}

	/**
	 * __fixdfdi - Convert double to signed 64-bit integer (truncate toward zero)
	 *
	 * Called by compiler for: (int64_t)double_value (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME INT64 __fixdfdi(double val)
	{
		UINT64 bits = __builtin_bit_cast(UINT64, val);
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
	 * __fixunsdfdi - Convert double to unsigned 64-bit integer (truncate toward zero)
	 *
	 * Called by compiler for: (uint64_t)double_value (on 32-bit RISC-V)
	 */
	COMPILER_RUNTIME UINT64 __fixunsdfdi(double val)
	{
		UINT64 bits = __builtin_bit_cast(UINT64, val);
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

#endif // ARCHITECTURE_RISCV32
