/**
 * @file prng.h
 * @brief Pseudorandom Number Generator (xorshift64)
 *
 * @details Platform-independent pseudorandom number generator using the
 * xorshift64 algorithm by George Marsaglia. Produces a full-period sequence
 * of 2^64-1 non-zero states from any non-zero seed.
 *
 * This class lives in the CORE layer and has no platform dependencies.
 * For automatic hardware-timestamp seeding, use the PLATFORM layer's
 * Random class, which wraps Prng.
 *
 * @see Marsaglia, G. (2003). "Xorshift RNGs". Journal of Statistical Software, 8(14), 1-6.
 *
 * @ingroup core
 *
 * @defgroup prng Pseudorandom Number Generator
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"

/**
 * @class Prng
 * @brief Deterministic xorshift64 pseudorandom number generator
 *
 * @details All methods are force-inlined for maximum performance. The generator
 * requires an explicit non-zero seed; use IsSeeded() to check initialization.
 *
 * @par Example Usage:
 * @code
 * Prng prng(12345);
 * INT32 val = prng.Get();              // Random INT32 in [0, Prng::Max]
 * UINT8 buf[16];
 * prng.GetArray(Span<UINT8>(buf, 16)); // Fill buffer with random bytes
 * CHAR c = prng.GetChar<CHAR>();       // Random lowercase letter
 * @endcode
 */
class Prng
{
private:
	UINT64 state;

public:
	/// @name Heap Operators
	/// @{

	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/// @}

	/// The inclusive upper bound for values returned by Get()
	static constexpr INT32 Max = 0x7FFFFFFF;

	/**
	 * @brief Default constructor — unseeded state
	 * @details State is zero (unseeded). Call Seed() before using Get().
	 */
	constexpr Prng() : state(0) {}

	/**
	 * @brief Construct with an explicit seed
	 * @param seed Initial state (should be non-zero for meaningful output)
	 */
	constexpr explicit Prng(UINT64 seed) : state(seed) {}

	/**
	 * @brief Set or reset the generator state
	 * @param seed New state value
	 */
	constexpr FORCE_INLINE VOID Seed(UINT64 seed) { state = seed; }

	/**
	 * @brief Check whether the generator has been seeded
	 * @return true if state is non-zero
	 */
	constexpr FORCE_INLINE BOOL IsSeeded() const { return state != 0; }

	/**
	 * @brief Generate the next pseudorandom number
	 *
	 * @details Advances the xorshift64 state and returns the lower 31 bits
	 * as a non-negative INT32 in the range [0, Max].
	 *
	 * @return Pseudorandom INT32 in [0, 0x7FFFFFFF]
	 */
	constexpr FORCE_INLINE INT32 Get()
	{
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return static_cast<INT32>(state & 0x7FFFFFFF);
	}

	/**
	 * @brief Fill a buffer with pseudorandom bytes
	 * @param buffer Output buffer to fill
	 */
	constexpr FORCE_INLINE VOID GetArray(Span<UINT8> buffer)
	{
		for (USIZE i = 0; i < buffer.Size(); ++i)
			buffer[i] = (UINT8)(Get() & 0xFF);
	}

	/**
	 * @brief Generate a random lowercase letter (a-z)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @return Random character in ['a', 'z']
	 */
	template <typename TChar>
	constexpr FORCE_INLINE TChar GetChar()
	{
		INT32 val = Get();
		// Map [0, 32767] to [0, 25] using: (val * 26) / 32768
		INT32 charOffset = ((val & 0x7FFF) * 26) >> 15;
		if (charOffset > 25)
			charOffset = 25;
		return (TChar)('a' + charOffset);
	}

	/**
	 * @brief Fill a span with random lowercase characters and null-terminate
	 *
	 * @details Writes str.Size()-1 random characters followed by a null
	 * terminator. Returns the number of random characters written.
	 *
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str Output span (must include space for null terminator)
	 * @return Number of random characters written (str.Size()-1), or 0 if empty
	 */
	template <typename TChar>
	constexpr FORCE_INLINE UINT32 GetString(Span<TChar> str)
	{
		if (str.Size() == 0)
			return 0;
		UINT32 length = (UINT32)str.Size() - 1;
		for (UINT32 i = 0; i < length; i++)
			str[i] = GetChar<TChar>();
		str[length] = (TChar)'\0';
		return length;
	}
};

/** @} */ // end of prng group
