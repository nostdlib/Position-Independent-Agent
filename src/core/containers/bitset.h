/**
 * @file bitset.h
 * @brief Heap-Backed Bitset
 *
 * @details Provides a compact, dynamically sized bit array over a heap
 * UINT8[] backing store. One bit per index gives an 8x memory reduction over
 * the heap BOOL[] grids it replaces (a 1080p tile grid drops from
 * tiles * 4 bytes to tiles / 8 bytes).
 *
 * Key properties:
 * - RAII: destructor frees the backing array
 * - Move-only: copy is deleted, move transfers ownership
 * - Fallible: Init() returns Result<VOID, Error> (Err on allocation failure
 *   or invalid state)
 * - Stack-only: heap allocation of the Bitset itself is deleted
 *
 * @ingroup core
 *
 * @defgroup bitset Bitset
 * @ingroup core
 * @{
 */

#pragma once

#include "core/memory/memory.h"
#include "core/types/primitives.h"
#include "core/types/result.h"

/**
 * @brief Dynamically sized bit array over a heap UINT8[] backing store
 *
 * @par Example Usage:
 * @code
 * Bitset visited;
 * if (!visited.Init(tileCount)) return;  // allocation failed
 * visited.Set(index);
 * if (visited.Test(index)) { ... }
 * // destructor frees automatically
 * @endcode
 */
struct Bitset
{
	UINT8 *Data;    ///< Backing byte array (bit i lives in Data[i / 8], bit i % 8)
	USIZE BitCount;

	// Stack-only: heap allocation of the Bitset itself is deleted
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	// Placement new/delete required by Result<Bitset, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	constexpr Bitset() : Data(nullptr), BitCount(0) {}

	~Bitset()
	{
		if (Data)
			delete[] Data;
	}

	Bitset(const Bitset &) = delete;
	Bitset &operator=(const Bitset &) = delete;

	constexpr Bitset(Bitset &&other) : Data(other.Data), BitCount(other.BitCount)
	{
		other.Data = nullptr;
		other.BitCount = 0;
	}


	Bitset &operator=(Bitset &&other)
	{
		if (this != &other)
		{
			if (Data)
				delete[] Data;
			Data = other.Data;
			BitCount = other.BitCount;
			other.Data = nullptr;
			other.BitCount = 0;
		}
		return *this;
	}

	/**
	 * @brief Allocate zeroed storage for a number of bits
	 * @param bitCount Number of bits the set must address
	 * @return Ok on success; Err(Bitset_InvalidState) on re-init of an
	 *         initialized set, Err(Bitset_AllocationFailed) when allocation fails
	 * @note Allocates Ceil(bitCount / 8) bytes; all bits start cleared
	 */
	[[nodiscard]] Result<VOID, Error> Init(USIZE bitCount)
	{
		if (Data)
			return Result<VOID, Error>::Err(Error::Bitset_InvalidState);
		if (bitCount > ((USIZE)-1) - 7)
			return Result<VOID, Error>::Err(Error::Bitset_InvalidState); // overflow
		USIZE byteCount = (bitCount + 7) / 8;
		Data = new UINT8[byteCount];
		if (!Data)
			return Result<VOID, Error>::Err(Error::Bitset_AllocationFailed);
		Memory::Zero(Data, byteCount);
		BitCount = bitCount;
		return Result<VOID, Error>::Ok();
	}


	VOID Set(USIZE index)
	{
		Data[index >> 3] |= (UINT8)(1u << (index & 7));
	}


	VOID Clear(USIZE index)
	{
		Data[index >> 3] &= (UINT8)~(1u << (index & 7));
	}

	/**
	 * @brief Read a bit
	 * @param index Bit index (unchecked; must be < BitCount)
	 * @return true if the bit is set, false if clear
	 */
	BOOL Test(USIZE index) const
	{
		return (Data[index >> 3] & (1u << (index & 7))) != 0;
	}

	/**
	 * @brief Clear every bit without releasing the allocation
	 * @note Retaining the backing store makes reuse free — useful for per-frame grids
	 */
	VOID Reset()
	{
		if (Data && BitCount > 0)
			Memory::Zero(Data, (BitCount + 7) / 8);
	}
};


