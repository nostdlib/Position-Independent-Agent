/**
 * @file uuid.h
 * @brief Universally Unique Identifier (UUID) Generation and Manipulation
 *
 * @details Provides UUID generation (version 4, random), parsing from string
 * representation, and serialization to the standard 8-4-4-4-12 hex format.
 *
 * @see RFC 9562 — Universally Unique IDentifiers (UUIDs)
 *      https://datatracker.ietf.org/doc/html/rfc9562
 *
 * @ingroup core
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/types/error.h"
#include "core/types/result.h"
#include "core/string/string.h"

/**
 * @class UUID
 * @brief 128-bit Universally Unique Identifier
 *
 * @details Stores a 128-bit UUID as a 16-byte array and provides factory
 * methods for random generation and string parsing.
 */
class UUID
{
private:
	UINT8 data[16];

public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/// @name Constructors
	/// @{

	/**
	 * @brief Default constructor — initializes to nil UUID (all zeros)
	 */
	constexpr UUID() noexcept : data{}
	{
	}

	/**
	 * @brief Construct from a 16-byte span
	 * @param bytes Exactly 16 bytes of UUID data
	 */
	constexpr UUID(Span<const UINT8, 16> bytes) noexcept : data{}
	{
		for (USIZE i = 0; i < 16; i++)
			data[i] = bytes[i];
	}

	/// @}
	/// @name Factory Methods
	/// @{

	/**
	 * @brief Parse a UUID from a string span
	 * @param str UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
	 * @return Ok(UUID) on success, Err(Uuid_FromStringFailed) if the string
	 *         does not contain exactly 32 hex digits
	 */
	[[nodiscard]] static Result<UUID, Error> FromString(Span<const CHAR> str) noexcept
	{
		UINT8 bytes[16]{};
		INT32 byteIndex = 0;
		INT32 nibbleCount = 0;

		for (USIZE i = 0; i < str.Size() && str[i] != '\0'; i++)
		{
			CHAR c = str[i];
			if (c == '-')
				continue;

			UINT8 value;
			if (c >= '0' && c <= '9')
				value = c - '0';
			else if (c >= 'a' && c <= 'f')
				value = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				value = c - 'A' + 10;
			else
				return Result<UUID, Error>::Err(Error::Uuid_FromStringFailed);

			if (byteIndex >= 16)
				return Result<UUID, Error>::Err(Error::Uuid_FromStringFailed);

			if (nibbleCount == 0)
			{
				bytes[byteIndex] = value << 4;
				nibbleCount = 1;
			}
			else
			{
				bytes[byteIndex] |= value;
				byteIndex++;
				nibbleCount = 0;
			}
		}

		if (byteIndex != 16 || nibbleCount != 0)
			return Result<UUID, Error>::Err(Error::Uuid_FromStringFailed);

		return Result<UUID, Error>::Ok(UUID(Span<const UINT8, 16>(bytes)));
	}

	/**
	 * @brief Parse a UUID from a null-terminated string
	 * @param str Null-terminated UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
	 * @return Ok(UUID) on success, Err(Uuid_FromStringFailed) if the string
	 *         does not contain exactly 32 hex digits
	 */
	[[nodiscard]] static Result<UUID, Error> FromString(PCCHAR str) noexcept
	{
		return FromString(Span<const CHAR>(str, StringUtils::Length(str)));
	}

	/// @}
	/// @name Serialization
	/// @{

	/**
	 * @brief Convert UUID to its standard string representation
	 * @param buffer Output buffer (must be at least 37 bytes: 32 hex + 4 dashes + null)
	 * @return Ok on success, Err if the buffer is too small
	 */
	[[nodiscard]] Result<VOID, Error> ToString(Span<CHAR> buffer) const noexcept
	{
		if (buffer.Size() < 37)
			return Result<VOID, Error>::Err(Error::Uuid_ToStringFailed);

		INT32 index = 0;
		auto hex = "0123456789abcdef";

		for (INT32 i = 0; i < 16; i++)
		{
			buffer[index++] = hex[(USIZE)((data[i] >> 4) & 0xF)];
			buffer[index++] = hex[(USIZE)(data[i] & 0xF)];
			if (i == 3 || i == 5 || i == 7 || i == 9)
				buffer[index++] = '-';
		}
		buffer[index] = '\0';

		return Result<VOID, Error>::Ok();
	}

	/// @}
	/// @name Accessors
	/// @{

	/**
	 * @brief Get the most significant 64 bits of the UUID
	 * @return Upper 64 bits (bytes 0-7)
	 */
	constexpr FORCE_INLINE UINT64 GetMostSignificantBits() const noexcept
	{
		UINT64 msb = 0;
		for (INT32 i = 0; i < 8; i++)
			msb = (msb << 8) | data[i];
		return msb;
	}

	/**
	 * @brief Get the least significant 64 bits of the UUID
	 * @return Lower 64 bits (bytes 8-15)
	 */
	constexpr FORCE_INLINE UINT64 GetLeastSignificantBits() const noexcept
	{
		UINT64 lsb = 0;
		for (INT32 i = 8; i < 16; i++)
			lsb = (lsb << 8) | data[i];
		return lsb;
	}

	/// @}
};
