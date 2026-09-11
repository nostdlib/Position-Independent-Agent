/**
 * @file binary_writer.h
 * @brief Sequential Binary Data Writer
 *
 * @details Provides a position-tracked, bounds-checked writer for sequential
 * binary data serialization. Used throughout the runtime for constructing network
 * protocol messages, certificate structures, and binary file formats.
 *
 * The writer maintains an internal offset that advances automatically as data
 * is written. All write operations perform bounds checking against a maximum
 * size to prevent buffer overruns.
 *
 * Multi-byte integer writes use big-endian (network) byte order, matching the
 * wire format of most Internet protocols.
 *
 * @see RFC 1700 — Assigned Numbers (network byte order convention)
 *      https://datatracker.ietf.org/doc/html/rfc1700
 *
 * @note All functions are position-independent with no .rdata dependencies.
 *
 * @ingroup core
 *
 * @defgroup binary_writer Binary Writer
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/memory/memory.h"

/**
 * @class BinaryWriter
 * @brief Sequential, bounds-checked binary data writer
 *
 * @details Wraps a raw memory buffer with a position cursor and maximum size.
 * Each write operation advances the cursor by the number of bytes written.
 * Out-of-bounds writes return nullptr rather than corrupting memory.
 *
 * Multi-byte writes (WriteU16BE, WriteU24BE, WriteU32BE) serialize in big-endian
 * (network) byte order as specified by RFC 1700.
 *
 * @par Example Usage:
 * @code
 * UINT8 packet[128];
 * BinaryWriter writer(Span<UINT8>(packet));
 *
 * writer.WriteU8(0x16);                 // Content type
 * writer.WriteU16BE(0x0303);            // TLS version
 * writer.WriteU24BE(payloadLength);     // Record length (24-bit)
 * writer.WriteBytes(Span(payload));     // Payload data
 * @endcode
 *
 * @see RFC 1700 — Assigned Numbers (network byte order convention)
 *      https://datatracker.ietf.org/doc/html/rfc1700
 */
class BinaryWriter
{
private:
	UINT8 *address;  ///< Base address of the output buffer
	USIZE offset;    ///< Current write position (bytes from base)
	USIZE maxSize;   ///< Maximum writable size in bytes

public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}         

	/**
	 * @brief Construct a writer from a mutable byte span with explicit initial offset
	 * @param data Span of bytes to write into
	 * @param offset Initial write offset in bytes
	 */
	constexpr BinaryWriter(Span<UINT8> data, USIZE offset)
		: address(data.Data()), offset(offset), maxSize(data.Size())
	{
	}

	/**
	 * @brief Construct a writer from a mutable byte span starting at offset zero
	 * @param data Span of bytes to write into
	 */
	constexpr BinaryWriter(Span<UINT8> data)
		: address(data.Data()), offset(0), maxSize(data.Size())
	{
	}
	/**
	 * @brief Write a value of type T and advance the cursor
	 * @tparam T Type to write (must be trivially copyable)
	 * @param value Value to write
	 * @return Base address on success, nullptr if insufficient space remains
	 *
	 * @details Copies sizeof(T) bytes from the value into the buffer at the
	 * current position using Memory::Copy. The cursor advances by sizeof(T)
	 * bytes on success.
	 */
	template <typename T>
	PVOID Write(T value)
	{
		if (sizeof(T) > maxSize - offset)
			return nullptr;

		Memory::Copy(address + offset, &value, sizeof(T));
		offset += sizeof(T);
		return address;
	}

	/**
	 * @brief Write raw bytes from a span and advance the cursor
	 * @param data Source data span
	 * @return Base address on success, nullptr if insufficient space remains
	 */
	PVOID WriteBytes(Span<const UINT8> data)
	{
		if (data.Size() > maxSize - offset)
			return nullptr;

		Memory::Copy(address + offset, data.Data(), data.Size());
		offset += data.Size();
		return address;
	}

	/**
	 * @brief Write a single byte and advance the cursor
	 * @param value Byte value to write
	 * @return Base address on success, nullptr if insufficient space remains
	 */
	constexpr FORCE_INLINE PVOID WriteU8(UINT8 value)
	{
		if (maxSize - offset < 1)
			return nullptr;

		*(address + offset) = value;
		offset += 1;
		return address;
	}

	/**
	 * @brief Write a 16-bit unsigned integer in big-endian (network) byte order
	 * @param value The 16-bit value to write
	 * @return Base address on success, nullptr if insufficient space remains
	 *
	 * @details Writes 2 bytes in big-endian order (MSB first),
	 * as specified by the network byte order convention in RFC 1700.
	 *
	 * @see RFC 1700 — Assigned Numbers (network byte order)
	 *      https://datatracker.ietf.org/doc/html/rfc1700
	 */
	constexpr FORCE_INLINE PVOID WriteU16BE(UINT16 value)
	{
		if (maxSize - offset < 2)
			return nullptr;

		UINT8 *p = address + offset;
		p[0] = (UINT8)(value >> 8);
		p[1] = (UINT8)(value & 0xFF);
		offset += 2;
		return address;
	}

	/**
	 * @brief Write a 24-bit unsigned integer in big-endian (network) byte order
	 * @param value The value to write (lower 24 bits used)
	 * @return Base address on success, nullptr if insufficient space remains
	 *
	 * @details Writes 3 bytes in big-endian order (MSB first).
	 * Common in TLS record headers where the record length is a 24-bit field.
	 *
	 * @see RFC 8446 Section 5.1 — Record Layer (TLS 1.3 record format uses 24-bit length)
	 *      https://datatracker.ietf.org/doc/html/rfc8446#section-5.1
	 */
	constexpr FORCE_INLINE PVOID WriteU24BE(UINT32 value)
	{
		if (maxSize - offset < 3)
			return nullptr;

		UINT8 *p = address + offset;
		p[0] = (UINT8)((value >> 16) & 0xFF);
		p[1] = (UINT8)((value >> 8) & 0xFF);
		p[2] = (UINT8)(value & 0xFF);
		offset += 3;
		return address;
	}

	/**
	 * @brief Write a 32-bit unsigned integer in big-endian (network) byte order
	 * @param value The 32-bit value to write
	 * @return Base address on success, nullptr if insufficient space remains
	 *
	 * @details Writes 4 bytes in big-endian order (MSB first),
	 * as specified by the network byte order convention in RFC 1700.
	 *
	 * @see RFC 1700 — Assigned Numbers (network byte order)
	 *      https://datatracker.ietf.org/doc/html/rfc1700
	 */
	constexpr FORCE_INLINE PVOID WriteU32BE(UINT32 value)
	{
		if (maxSize - offset < 4)
			return nullptr;

		UINT8 *p = address + offset;
		p[0] = (UINT8)((value >> 24) & 0xFF);
		p[1] = (UINT8)((value >> 16) & 0xFF);
		p[2] = (UINT8)((value >> 8) & 0xFF);
		p[3] = (UINT8)(value & 0xFF);
		offset += 4;
		return address;
	}

	/**
	 * @brief Write a null-terminated string's bytes (no terminator) and advance
	 * @param str Null-terminated string to write
	 * @return Base address on success, nullptr if insufficient space remains
	 *
	 * @details Copies characters up to but not including the terminating null,
	 * so callers control whether a terminator follows. The full length is
	 * measured first; on overflow nothing is written and the cursor does not
	 * move, matching the all-or-nothing behaviour of WriteBytes.
	 */
	constexpr FORCE_INLINE PVOID WriteString(const CHAR *str)
	{
		if (str == nullptr)
			return nullptr;
		if (offset > maxSize)
			return nullptr;

		USIZE length = 0;
		while (str[length] != '\0')
			length += 1;

		if (length > maxSize - offset)
			return nullptr;

		Memory::Copy(address + offset, str, length);
		offset += length;
		return address;
	}

	/**
	 * @brief Skip forward by a number of bytes (leaves bytes unwritten)
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
	 * @brief Get the number of writable bytes remaining
	 * @return Bytes remaining from current offset to maxSize
	 */
	constexpr USIZE Remaining() const
	{
		return (offset < maxSize) ? (maxSize - offset) : 0;
	}

	constexpr UINT8 *GetAddress() const { return address; }
	constexpr USIZE GetOffset() const { return offset; }
	constexpr USIZE GetMaxSize() const { return maxSize; }
};

