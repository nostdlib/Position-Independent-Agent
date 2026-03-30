/**
 * @file ip_address.h
 * @brief IP Address Type Supporting IPv4 and IPv6
 *
 * @details Provides a unified IP address type that supports both IPv4 and IPv6
 * addresses. Designed for network programming in position-independent code
 * without CRT dependencies.
 *
 * Features:
 * - Unified IPv4/IPv6 representation
 * - Fully constexpr construction and comparison (no .rdata generation)
 * - String parsing and formatting (dotted-decimal for IPv4, colon-hex for IPv6)
 * - Factory methods for construction
 * - Comparison operators
 *
 * @see RFC 791 — Internet Protocol (IPv4 addressing)
 *      https://datatracker.ietf.org/doc/html/rfc791
 * @see RFC 8200 — Internet Protocol, Version 6 (IPv6) Specification
 *      https://datatracker.ietf.org/doc/html/rfc8200
 * @see RFC 5952 — A Recommendation for IPv6 Address Text Representation
 *      https://datatracker.ietf.org/doc/html/rfc5952
 * @see RFC 4291 Section 2.2 — Text Representation of Addresses
 *      https://datatracker.ietf.org/doc/html/rfc4291#section-2.2
 *
 * @ingroup core
 *
 * @defgroup ip_address IP Address
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/types/error.h"
#include "core/types/result.h"

/**
 * @enum IPVersion
 * @brief IP address version enumeration
 */
enum class IPVersion : UINT8
{
	IPv4 = 4,   ///< IPv4 address (32-bit)
	IPv6 = 6,   ///< IPv6 address (128-bit)
	Invalid = 0 ///< Invalid/uninitialized address
};

/**
 * @class IPAddress
 * @brief Unified IP address class supporting both IPv4 and IPv6
 *
 * @details Uses a union to efficiently store either an IPv4 (4 bytes) or IPv6
 * (16 bytes) address. Factory methods provide type-safe construction.
 * All constructors, factory methods, and operators are constexpr, enabling
 * compile-time evaluation that avoids .rdata generation.
 *
 * @par Example Usage:
 * @code
 * // Compile-time IPv4 address (no .rdata)
 * constexpr IPAddress loopback = IPAddress::FromIPv4(0x0100007F);
 * static_assert(loopback.IsIPv4());
 * static_assert(loopback.ToIPv4() == 0x0100007F);
 *
 * // Create IPv4 address from string (runtime only)
 * auto addrResult = IPAddress::FromString("192.168.1.1");
 * if (!addrResult)
 *     return;
 * auto& addr = addrResult.Value();
 * if (addr.IsIPv4()) {
 *     UINT32 ip = addr.ToIPv4();  // Network byte order
 * }
 *
 * // Convert to string
 * CHAR buffer[64];
 * addr.ToString(Span<CHAR>(buffer));
 * @endcode
 */
class IPAddress
{
private:
	IPVersion version; ///< Address version (IPv4, IPv6, or Invalid)
	union
	{
		UINT32 ipv4;    ///< IPv4 address (4 bytes, network byte order)
		UINT8 ipv6[16]; ///< IPv6 address (16 bytes)
	} address;

private:
	/**
	 * @brief Private constructor for IPv4 address
	 * @param ipv4Address IPv4 address in network byte order
	 */
	constexpr IPAddress(UINT32 ipv4Address) : version(IPVersion::IPv4), address{}
	{
		address.ipv4 = ipv4Address;
	}

	/**
	 * @brief Private constructor for IPv6 address
	 * @param ipv6Address 16-byte IPv6 address array
	 */
	constexpr IPAddress(const UINT8 (&ipv6Address)[16]) : version(IPVersion::IPv6), address{}
	{
		for (USIZE i = 0; i < 16; ++i)
			address.ipv6[i] = ipv6Address[i];
	}

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
	 * @brief Default constructor - creates an invalid IP address
	 */
	constexpr IPAddress() : version(IPVersion::Invalid), address{} {}

	/**
	 * @brief Copy constructor
	 * @param other IPAddress to copy
	 */
	constexpr IPAddress(const IPAddress &other) : version(other.version), address{}
	{
		if (version == IPVersion::IPv4)
		{
			address.ipv4 = other.address.ipv4;
		}
		else if (version == IPVersion::IPv6)
		{
			for (USIZE i = 0; i < 16; ++i)
				address.ipv6[i] = other.address.ipv6[i];
		}
	}

	/// @}
	/// @name Factory Methods
	/// @{

	/**
	 * @brief Create IPv4 address from raw 32-bit value
	 * @param ipv4Address IPv4 address in network byte order
	 * @return IPAddress instance
	 */
	static constexpr IPAddress FromIPv4(UINT32 ipv4Address) { return IPAddress(ipv4Address); }

	/**
	 * @brief Create IPv6 address from 16-byte array
	 * @param ipv6Address 16-byte IPv6 address
	 * @return IPAddress instance
	 */
	static constexpr IPAddress FromIPv6(const UINT8 (&ipv6Address)[16]) { return IPAddress(ipv6Address); }

	/**
	 * @brief Parse IP address from string span
	 * @param ipString String span representation (e.g., "192.168.1.1" or "::1")
	 * @return Ok(IPAddress) on success, Err(IpAddress_ParseFailed) on failure
	 */
	[[nodiscard]] static Result<IPAddress, Error> FromString(Span<const CHAR> ipString);

	/**
	 * @brief Parse IP address from null-terminated string
	 * @param ipString Null-terminated string representation (e.g., "192.168.1.1" or "::1")
	 * @return Ok(IPAddress) on success, Err(IpAddress_ParseFailed) on failure
	 */
	[[nodiscard]] static Result<IPAddress, Error> FromString(PCCHAR ipString);

	/**
	 * @brief Get loopback address (127.0.0.1 for IPv4, ::1 for IPv6)
	 * @param ipv6 true for IPv6 loopback, false for IPv4 loopback
	 * @return IPAddress instance representing the loopback address
	 *
	 * @see RFC 791 — 127.0.0.1 loopback address
	 *      https://datatracker.ietf.org/doc/html/rfc791
	 * @see RFC 4291 Section 2.5.3 — The Loopback Address (::1)
	 *      https://datatracker.ietf.org/doc/html/rfc4291#section-2.5.3
	 */
	static constexpr IPAddress LocalHost(BOOL ipv6 = false)
	{
		if (ipv6)
		{
			UINT8 loopbackV6[16]{};
			loopbackV6[15] = 1;
			return IPAddress(loopbackV6);
		}
		return IPAddress(static_cast<UINT32>(0x0100007F));
	}

	/**
	 * @brief Create an invalid IP address
	 * @return IPAddress with Invalid version
	 */
	static constexpr IPAddress Invalid() { return IPAddress(); }

	/// @}
	/// @name Validation
	/// @{

	/**
	 * @brief Check if address is valid
	 * @return true if IPv4 or IPv6, false if Invalid
	 */
	constexpr BOOL IsValid() const { return version != IPVersion::Invalid; }

	/**
	 * @brief Check if address is IPv4
	 * @return true if IPv4 address
	 */
	constexpr BOOL IsIPv4() const { return version == IPVersion::IPv4; }

	/**
	 * @brief Check if address is IPv6
	 * @return true if IPv6 address
	 */
	constexpr BOOL IsIPv6() const { return version == IPVersion::IPv6; }

	/**
	 * @brief Get address version
	 * @return IPVersion enumeration value
	 */
	constexpr IPVersion GetVersion() const { return version; }

	/// @}
	/// @name Conversion Methods
	/// @{

	/**
	 * @brief Get IPv4 address value
	 * @return IPv4 address in network byte order (0xFFFFFFFF if not IPv4)
	 */
	constexpr UINT32 ToIPv4() const { return version == IPVersion::IPv4 ? address.ipv4 : 0xFFFFFFFF; }

	/**
	 * @brief Get IPv6 address array
	 * @return Pointer to 16-byte IPv6 address (nullptr if not IPv6)
	 */
	constexpr const UINT8 *ToIPv6() const { return version == IPVersion::IPv6 ? address.ipv6 : nullptr; }

	/**
	 * @brief Convert to string representation
	 * @param buffer Output buffer span
	 * @return Result<VOID, Error> - Ok on success, Err(IpAddress_ToStringFailed) if buffer too small or invalid
	 */
	[[nodiscard]] Result<VOID, Error> ToString(Span<CHAR> buffer) const;

	/// @}
	/// @name Operators
	/// @{

	/**
	 * @brief Equality comparison
	 * @param other IPAddress to compare
	 * @return true if addresses are equal
	 */
	constexpr BOOL operator==(const IPAddress &other) const
	{
		if (version != other.version)
			return false;
		if (version == IPVersion::IPv4)
			return address.ipv4 == other.address.ipv4;
		if (version == IPVersion::IPv6)
		{
			for (USIZE i = 0; i < 16; ++i)
			{
				if (address.ipv6[i] != other.address.ipv6[i])
					return false;
			}
			return true;
		}
		return true; // Both Invalid
	}

	/**
	 * @brief Inequality comparison
	 * @param other IPAddress to compare
	 * @return true if addresses are not equal
	 */
	constexpr BOOL operator!=(const IPAddress &other) const { return !(*this == other); }

	/**
	 * @brief Assignment operator
	 * @param other IPAddress to assign
	 * @return Reference to this
	 */
	constexpr IPAddress &operator=(const IPAddress &other)
	{
		if (this != &other)
		{
			version = other.version;
			if (version == IPVersion::IPv4)
			{
				address.ipv4 = other.address.ipv4;
			}
			else if (version == IPVersion::IPv6)
			{
				for (USIZE i = 0; i < 16; ++i)
					address.ipv6[i] = other.address.ipv6[i];
			}
		}
		return *this;
	}

	/// @}
};

/** @} */ // end of ip_address group
