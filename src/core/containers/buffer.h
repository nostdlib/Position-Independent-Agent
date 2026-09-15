/**
 * @file buffer.h
 * @brief Owning Dynamic Byte/Element Buffer
 *
 * @details Provides a generic, owning dynamic buffer with USIZE-sized counts
 * and exponential growth. Vector<T> uses INT32 counts, capping it at 2 GB —
 * too tight for screenshot and file payloads. Buffer<T> keeps the same
 * ownership model but scales to the platform word size.
 *
 * Key properties:
 * - RAII: destructor frees the backing array
 * - Move-only: copy is deleted, move transfers ownership
 * - Fallible: growth members return Result<VOID, Error> (Err on allocation
 *   failure or invalid state)
 * - Stack-only: heap allocation of the Buffer itself is deleted
 *
 * @ingroup core
 *
 * @defgroup buffer Buffer
 * @ingroup core
 * @{
 */

#pragma once

#include "core/memory/memory.h"
#include "core/types/primitives.h"
#include "core/types/result.h"
#include "core/types/span.h"

static constexpr USIZE BufferInitialCapacity = 16;

/**
 * @brief Owning dynamic buffer with USIZE counts and exponential growth
 *
 * @tparam T Element type (must be trivially copyable); defaults to UINT8
 *
 * @par Example Usage:
 * @code
 * Buffer<UINT8> b;
 * if (!b.Init(1024)) return;          // failed — inspect b via the returned Result's Error()
 * if (!b.Append(Span<const UINT8>(src, n))) return;
 * Memory::Copy(dst, b.AsSpan().Data(), b.AsSpan().Size());
 * // destructor frees automatically
 * @endcode
 */
template <typename T = UINT8>
struct Buffer
{
	T *Data;      ///< Backing array (null when empty)
	USIZE Capacity;
	USIZE Size;

	// Stack-only: heap allocation of the Buffer itself is deleted
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	// Placement new/delete required by Result<Buffer<T>, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	constexpr Buffer() : Data(nullptr), Capacity(0), Size(0) {}

	~Buffer()
	{
		if (Data)
			delete[] Data;
	}

	Buffer(const Buffer &) = delete;
	Buffer &operator=(const Buffer &) = delete;

	constexpr Buffer(Buffer &&other) : Data(other.Data), Capacity(other.Capacity), Size(other.Size)
	{
		other.Data = nullptr;
		other.Capacity = 0;
		other.Size = 0;
	}

	/**
	 * @brief Move-assign, freeing any array this buffer already owns
	 * @param other Buffer to steal from (left empty)
	 * @return Reference to this buffer
	 */
	Buffer &operator=(Buffer &&other)
	{
		if (this != &other)
		{
			if (Data)
				delete[] Data;
			Data = other.Data;
			Capacity = other.Capacity;
			Size = other.Size;
			other.Data = nullptr;
			other.Capacity = 0;
			other.Size = 0;
		}
		return *this;
	}

	/**
	 * @brief Allocate a backing array of the requested capacity
	 * @param capacity Element count to allocate
	 * @return Ok on success; Err(Buffer_InvalidState) on re-init of a non-empty
	 *         buffer, Err(Buffer_AllocationFailed) when allocation fails
	 * @note Size is left at 0 — the buffer starts logically empty
	 */
	[[nodiscard]] Result<VOID, Error> Init(USIZE capacity)
	{
		if (Data)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState);
		Data = new T[capacity];
		if (!Data)
			return Result<VOID, Error>::Err(Error::Buffer_AllocationFailed);
		Capacity = capacity;
		Size = 0;
		return Result<VOID, Error>::Ok();
	}

	/**
	 * @brief Append one element, growing the backing array if needed
	 * @param value Element to append
	 * @return Ok on success; Err(Buffer_AllocationFailed) or Err(Buffer_InvalidState) from growth
	 */
	[[nodiscard]] Result<VOID, Error> Add(T value)
	{
		Result<VOID, Error> reserve = Reserve(1);
		if (!reserve)
			return Result<VOID, Error>::Err(reserve.Error());
		Data[Size] = value;
		Size += 1;
		return Result<VOID, Error>::Ok();
	}

	/**
	 * @brief Append a span of elements, growing the backing array if needed
	 * @param data Elements to append
	 * @return Ok on success; Err(Buffer_AllocationFailed) or Err(Buffer_InvalidState) from growth
	 */
	[[nodiscard]] Result<VOID, Error> Append(Span<const T> data)
	{
		Result<VOID, Error> reserve = Reserve(data.Size());
		if (!reserve)
			return Result<VOID, Error>::Err(reserve.Error());
		Memory::Copy(Data + Size, data.Data(), sizeof(T) * data.Size());
		Size += data.Size();
		return Result<VOID, Error>::Ok();
	}

	/**
	 * @brief Ensure room for a number of additional elements without writing them
	 * @param extra Element count that must fit beyond Size
	 * @return Ok on success; Err(Buffer_InvalidState) on USIZE overflow,
	 *         Err(Buffer_AllocationFailed) when allocation fails
	 * @note Growth policy is `Capacity ? Capacity * 2 : 16`, doubled until the
	 *       request fits — matching Vector and TlsBuffer::CheckSize
	 */
	[[nodiscard]] Result<VOID, Error> Reserve(USIZE extra)
	{
		USIZE required = Size + extra;
		if (required < Size)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState); // overflow
		if (required <= Capacity)
			return Result<VOID, Error>::Ok();

		USIZE newCapacity = Capacity ? Capacity * 2 : BufferInitialCapacity;
		if (newCapacity < Capacity)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState); // overflow
		while (newCapacity < required)
		{
			USIZE doubled = newCapacity * 2;
			if (doubled < newCapacity)
				return Result<VOID, Error>::Err(Error::Buffer_InvalidState); // overflow
			newCapacity = doubled;
		}

		T *newData = new T[newCapacity];
		if (!newData)
			return Result<VOID, Error>::Err(Error::Buffer_AllocationFailed);
		Memory::Copy(newData, Data, sizeof(T) * Size);
		delete[] Data;
		Data = newData;
		Capacity = newCapacity;
		return Result<VOID, Error>::Ok();
	}

	/**
	 * @brief Set the logical size to an absolute value, growing if needed
	 * @param newSize New element count (may be smaller than Size)
	 * @return Ok on success; Err(Buffer_InvalidState) on size overflow,
	 *         Err(Buffer_AllocationFailed) when growth allocation fails
	 * @note Never shrinks the allocation; new tail bytes are unwritten
	 */
	[[nodiscard]] Result<VOID, Error> Resize(USIZE newSize)
	{
		if (newSize > Capacity)
		{
			Result<VOID, Error> reserve = Reserve(newSize - Size);
			if (!reserve)
				return Result<VOID, Error>::Err(reserve.Error());
		}
		Size = newSize;
		return Result<VOID, Error>::Ok();
	}

	/**
	 * @brief Mark the buffer logically empty, keeping the allocation
	 * @note Retaining the capacity makes Reset() free and avoids re-allocation
	 *       on the next fill — useful for per-frame buffers
	 */
	VOID Reset()
	{
		Size = 0;
	}

	/**
	 * @brief Detach ownership of the backing array
	 * @return Pointer to the array, or nullptr if the buffer owns nothing
	 * @note The caller becomes responsible for `delete[]` on the returned pointer
	 */
	T *Release()
	{
		T *ptr = Data;
		Data = nullptr;
		Capacity = 0;
		Size = 0;
		return ptr;
	}

	/**
	 * @brief View the valid elements
	 * @return Span bounded by Size
	 */
	constexpr Span<T> AsSpan() const
	{
		return Span<T>(Data, Size);
	}

	/**
	 * @brief View the unwritten tail, for direct writes without going through Add
	 * @return Span of Capacity - Size elements starting at Data + Size
	 * @note The caller must keep Size in sync after writing into this region
	 */
	constexpr Span<T> Unused() const
	{
		return Span<T>(Data + Size, Capacity - Size);
	}
};

/** @} */ // end of buffer group
