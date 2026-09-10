/**
 * @file binary_reader.h
 * @brief Sequential Binary Data Reader
 *
 * @details Provides a position-tracked, bounds-checked reader for sequential
 * binary data parsing. Used throughout the runtime for deserializing network
 * protocol messages, certificate structures, and binary file formats.
 *
 * The reader maintains an internal offset that advances automatically as data
 * is consumed. All read operations perform bounds checking against a maximum
 * size to prevent buffer overruns.
 *
 * Multi-byte integer reads use big-endian (network) byte order, matching the
 * wire format of most Internet protocols.
 *
 * @see RFC 1700 — Assigned Numbers (network byte order convention)
 *      https://datatracker.ietf.org/doc/html/rfc1700
 *
 * @note All functions are position-independent with no .rdata dependencies.
 *
 * @ingroup core
 *
 * @defgroup binary_reader Binary Reader
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/memory/memory.h"

/**
 * @class BinaryReader
 * @brief Sequential, bounds-checked binary data reader
 *
 * @details Wraps a raw memory buffer with a position cursor and maximum size.
 * Each read operation advances the cursor by the number of bytes consumed.
 * Out-of-bounds reads return zero/false rather than accessing invalid memory.
 *
 * Multi-byte reads (ReadU16BE, ReadU24BE, ReadU32BE) deserialize in big-endian
 * (network) byte order as specified by RFC 1700.
 *
 * @par Example Usage:
 * @code
 * UINT8 packet[128];
 * // ... fill packet from network ...
 * BinaryReader reader(Span<const UINT8>(packet));
 *
 * UINT16 type   = reader.ReadU16BE();   // 2 bytes, big-endian
 * UINT32 length = reader.ReadU24BE();   // 3 bytes, big-endian
 * reader.Skip(length);                  // Skip payload
 * @endcode
 *
 * @see RFC 1700 — Assigned Numbers (network byte order convention)
 *      https://datatracker.ietf.org/doc/html/rfc1700
 */
class BinaryReader
{
private:
	const UINT8 *address; 
	USIZE offset;         
	USIZE maxSize; 

public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; } 
	VOID operator delete(VOID *, PVOID) noexcept {}

	/**
	 * @brief Construct a reader from a byte span with explicit initial offset
	 * @param data Span of bytes to read from
	 * @param offset Initial read offset in bytes
	 */
	constexpr BinaryReader(Span<const UINT8> data, USIZE offset)
		: address(data.Data()), offset(offset), maxSize(data.Size())
	{
	}

	/**
	 * @brief Construct a reader from a byte span starting at offset zero
	 * @param data Span of bytes to read from
	 */
	constexpr BinaryReader(Span<const UINT8> data)
		: address(data.Data()), offset(0), maxSize(data.Size())
	{
	}
	/**
	 * @brief Read a value of type T and advance the cursor
	 * @tparam T Type to read (must be trivially copyable)
	 * @return The read value, or T{} if insufficient bytes remain
	 *
	 * @details Copies sizeof(T) bytes from the current position into a
	 * local value using Memory::Copy (no alignment requirements on the source).
	 * The cursor advances by sizeof(T) bytes on success.
	 */
	template <typename T>
	T Read()
	{
		if (sizeof(T) > maxSize - offset)
			return T{};

		T value;
		Memory::Copy(&value, address + offset, sizeof(T));
		offset += sizeof(T);
		return value;
	}

	/**
	 * @brief Read raw bytes into a buffer and advance the cursor
	 * @param buffer Destination buffer span
	 * @return Number of bytes read (buffer.Size() on success, 0 if out of bounds)
	 */
	USIZE ReadBytes(Span<UINT8> buffer)
	{
		if (buffer.Size() > maxSize - offset)
			return 0;

		Memory::Copy(buffer.Data(), address + offset, buffer.Size());
		offset += buffer.Size();
		return buffer.Size();
	}

	/**
	 * @brief Read a 16-bit unsigned integer in big-endian (network) byte order
	 * @return The 16-bit value, or 0 if insufficient bytes remain
	 *
	 * @details Reads 2 bytes and assembles them in big-endian order (MSB first),
	 * as specified by the network byte order convention in RFC 1700.
	 *
	 * @see RFC 1700 — Assigned Numbers (network byte order)
	 *      https://datatracker.ietf.org/doc/html/rfc1700
	 */
	constexpr FORCE_INLINE UINT16 ReadU16BE()
	{
		if (maxSize - offset < 2)
			return 0;

		const UINT8 *p = address + offset;
		UINT16 value = (UINT16)((p[0] << 8) | p[1]);
		offset += 2;
		return value;
	}

	/**
	 * @brief Read a 24-bit unsigned integer in big-endian (network) byte order
	 * @return The 24-bit value stored in a UINT32, or 0 if insufficient bytes remain
	 *
	 * @details Reads 3 bytes and assembles them in big-endian order (MSB first).
	 * Common in TLS record headers where the record length is a 24-bit field.
	 *
	 * @see RFC 8446 Section 5.1 — Record Layer (TLS 1.3 record format uses 24-bit length)
	 *      https://datatracker.ietf.org/doc/html/rfc8446#section-5.1
	 */
	constexpr FORCE_INLINE UINT32 ReadU24BE()
	{
		if (maxSize - offset < 3)
			return 0;

		const UINT8 *p = address + offset;
		UINT32 value = ((UINT32)p[0] << 16) | ((UINT32)p[1] << 8) | (UINT32)p[2];
		offset += 3;
		return value;
	}

	/**
	 * @brief Read a 32-bit unsigned integer in big-endian (network) byte order
	 * @return The 32-bit value, or 0 if insufficient bytes remain
	 *
	 * @details Reads 4 bytes and assembles them in big-endian order (MSB first),
	 * as specified by the network byte order convention in RFC 1700.
	 *
	 * @see RFC 1700 — Assigned Numbers (network byte order)
	 *      https://datatracker.ietf.org/doc/html/rfc1700
	 */
	constexpr FORCE_INLINE UINT32 ReadU32BE()
	{
		if (maxSize - offset < 4)
			return 0;

		const UINT8 *p = address + offset;
		UINT32 value = ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | (UINT32)p[3];
		offset += 4;
		return value;
	}

	/**
	 * @brief Skip forward by a number of bytes
	 * @param count Number of bytes to skip
	 * @return true if skip succeeded, false if it would exceed maxSize
	 */
	constexpr FORCE_INLINE BOOL Skip(USIZE count)
	{
		if (count > maxSize - offset)
			return false;

		offset += count;
		return true;
	}

	/**
	 * @brief Get the number of unread bytes remaining
	 * @return Bytes remaining from current offset to maxSize
	 */
	constexpr USIZE Remaining() const
	{
		return (offset < maxSize) ? (maxSize - offset) : 0;
	}

	/**
	 * @brief Set the read cursor to an absolute offset
	 * @param newOffset New offset in bytes from base address
	 * @return true if offset is valid, false if it exceeds maxSize
	 */
	constexpr FORCE_INLINE BOOL SetOffset(USIZE newOffset)
	{
		if (newOffset > maxSize)
			return false;

		offset = newOffset;
		return true;
	}

	/**
	 * @brief Get a pointer to the current read position
	 * @return Pointer to the byte at the current offset
	 */
	constexpr const UINT8 *Current() const
	{
		return address + offset;
	}

	constexpr const UINT8 *GetAddress() const { return address; }
	constexpr USIZE GetOffset() const { return offset; }
	constexpr USIZE GetMaxSize() const { return maxSize; }
};

/** @} */ // end of binary_reader group

