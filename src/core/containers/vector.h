/**
 * @file vector.h
 * @brief Owning Dynamic Array (Vector)
 *
 * @details Provides a generic, owning dynamic array with automatic memory
 * management via RAII. Replaces manual buffer management with
 * exponential-growth reallocation.
 *
 * Key properties:
 * - RAII: destructor frees the backing array
 * - Move-only: copy is deleted, move transfers ownership
 * - Fallible: Init() and Add() return BOOL (false on allocation failure)
 * - Stack-only: heap allocation of the Vector itself is deleted
 *
 * @ingroup runtime
 *
 * @defgroup vector Vector
 * @ingroup runtime
 * @{
 */

#pragma once

#include "core/memory/memory.h"
#include "core/types/primitives.h"


static constexpr INT32 VectorInitialCapacity = 10;

/**
 * @brief Owning dynamic array with exponential growth
 *
 * @tparam T Element type (must be trivially copyable)
 *
 * @par Example Usage:
 * @code
 * Vector<INT32> v;
 * if (!v.Init()) return; // allocation failed
 * if (!v.Add(42)) return; // grow failed
 * INT32 x = v.Data[0]; // direct access
 * // destructor frees automatically
 * @endcode
 */
template <typename T>
struct Vector
{
	T *Data;
	INT32 Capacity;
	INT32 Count;

	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;

	Vector() : Data(nullptr), Capacity(0), Count(0) {}

	~Vector()
	{
		if (Data)
			delete[] Data;
	}

	Vector(const Vector &) = delete;
	Vector &operator=(const Vector &) = delete;

	Vector(Vector &&other) : Data(other.Data), Capacity(other.Capacity), Count(other.Count)
	{
		other.Data = nullptr;
		other.Capacity = 0;
		other.Count = 0;
	}

	Vector &operator=(Vector &&other)
	{
		if (this != &other)
		{
			if (Data)
				delete[] Data;
			Data = other.Data;
			Capacity = other.Capacity;
			Count = other.Count;
			other.Data = nullptr;
			other.Capacity = 0;
			other.Count = 0;
		}
		return *this;
	}

	/**
	 * @brief Allocate the initial backing array
	 * @return true on success, false on allocation failure
	 */
	[[nodiscard]] BOOL Init()
	{
		Capacity = VectorInitialCapacity;
		Count = 0;
		Data = new T[Capacity];
		return Data != nullptr;
	}

	/**
	 * @brief Append an element, growing the array if needed
	 * @param value Element to append
	 * @return true on success, false on allocation failure
	 */
	[[nodiscard]] BOOL Add(T value)
	{
		if (Count + 1 >= Capacity)
		{
			INT32 newCapacity = Capacity * 2;
			T *newData = new T[newCapacity];
			if (newData == nullptr)
				return false;
			Memory::Copy(newData, Data, sizeof(T) * (USIZE)Count);
			delete[] Data;
			Data = newData;
			Capacity = newCapacity;
		}
		Data[Count] = value;
		Count += 1;
		return true;
	}

	/**
	 * @brief Release ownership of the backing array
	 * @return Pointer to the array (caller takes ownership)
	 */
	T *Release()
	{
		T *ptr = Data;
		Data = nullptr;
		Capacity = 0;
		Count = 0;
		return ptr;
	}
};

/** @} */ // end of vector group
