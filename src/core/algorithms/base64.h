/**
 * @file base64.h
 * @brief Base64 Encoding and Decoding
 *
 * @details Platform-independent Base64 encoding and decoding utilities implementing
 * the standard Base64 alphabet defined in RFC 4648 Section 4 ("Base 64 Encoding").
 * Converts arbitrary binary data into a printable ASCII representation using a
 * 65-character subset (A-Z, a-z, 0-9, +, /, =).
 *
 * Base64 is commonly used for:
 * - Encoding binary data in text-based protocols (HTTP, SMTP, MIME)
 * - Embedding binary data in JSON/XML
 * - Data URLs (RFC 2397)
 * - Cryptographic key and certificate encoding (PEM format)
 * - TLS 1.3 certificate messages
 *
 * @note Implementation uses branchless arithmetic character mapping instead of
 * lookup tables to avoid .rdata dependencies.
 *
 * @see RFC 4648 — The Base16, Base32, and Base64 Data Encodings
 *      https://datatracker.ietf.org/doc/html/rfc4648
 * @see RFC 4648 Section 4 — Base 64 Encoding (alphabet and padding definition)
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
 * @see RFC 4648 Section 3.5 — Canonical Encoding (padding requirements)
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.5
 *
 * @ingroup core
 *
 * @defgroup base64 Base64 Encoding
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/error.h"
#include "core/types/result.h"
#include "core/types/span.h"

/**
 * @class Base64
 * @brief Static class for Base64 encoding and decoding operations
 *
 * @details Provides position-independent Base64 encoding/decoding without
 * CRT dependencies. Uses the standard Base64 alphabet defined in Table 1
 * of RFC 4648 Section 4:
 *
 * - Indices  0–25 → 'A'–'Z'
 * - Indices 26–51 → 'a'–'z'
 * - Indices 52–61 → '0'–'9'
 * - Index   62    → '+'
 * - Index   63    → '/'
 * - Padding       → '='
 *
 * Encoding processes input in 3-byte (24-bit) groups, producing 4 Base64
 * characters per group (RFC 4648 Section 4). When the input length is not
 * a multiple of 3, the output is padded with '=' characters to align to a
 * 4-character boundary (RFC 4648 Section 3.5).
 *
 * @see RFC 4648 Section 4 — Base 64 Encoding (alphabet table and encoding procedure)
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
 * @see RFC 4648 Section 3.5 — Canonical Encoding (padding with '=')
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.5
 *
 * @par Example Usage:
 * @code
 * auto msg = "Hello, World!";
 * CHAR encoded[64];
 * Base64::Encode(Span(msg.Data(), msg.Length()), Span(encoded));
 * // encoded = "SGVsbG8sIFdvcmxkIQ=="
 *
 * CHAR decoded[64];
 * auto result = Base64::Decode(Span((const CHAR *)encoded, 20), Span(decoded));
 * // decoded = "Hello, World!"
 * @endcode
 */
class Base64
{
public:
	/**
	 * @brief Encodes binary data to Base64 format
	 *
	 * @details Implements the encoding procedure described in RFC 4648 Section 4.
	 * Input bytes are grouped into 24-bit blocks (3 bytes each). Each block is
	 * split into four 6-bit values, which are mapped to the Base64 alphabet.
	 *
	 * When the final block contains fewer than 3 bytes, padding is applied
	 * per RFC 4648 Section 3.5:
	 * - 1 remaining byte  → 2 encoded chars + "=="
	 * - 2 remaining bytes → 3 encoded chars + "="
	 *
	 * The output is null-terminated.
	 *
	 * @param input Input binary data
	 * @param output Output buffer (must be at least GetEncodeOutSize(input.Size()) bytes)
	 *
	 * @see RFC 4648 Section 4 — Base 64 Encoding (encoding procedure)
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
	 * @see RFC 4648 Section 3.5 — Canonical Encoding (padding rules)
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.5
	 */
	static VOID Encode(Span<const CHAR> input, Span<CHAR> output);

	/**
	 * @brief Decodes Base64 formatted data back to binary
	 *
	 * @details Implements the decoding procedure described in RFC 4648 Section 4.
	 * Input is processed in 4-character blocks. Each Base64 character is mapped
	 * back to its 6-bit value and the four values are combined into a 24-bit
	 * group, yielding 3 output bytes. Padding characters ('=') at the end
	 * indicate truncated output:
	 * - "xx==" → 1 decoded byte  (only the first 12 bits are significant)
	 * - "xxx=" → 2 decoded bytes (only the first 18 bits are significant)
	 *
	 * @param input Base64 encoded string (must be a multiple of 4 characters with valid padding)
	 * @param output Output buffer (must be at least GetDecodeOutSize(input.Size()) bytes)
	 * @return Result<void, Error> — Ok on success, Err on invalid input
	 *
	 * @see RFC 4648 Section 4 — Base 64 Encoding (decoding procedure)
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
	 * @see RFC 4648 Section 3.3 — Interpretation of Non-Alphabet Characters
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.3
	 */
	[[nodiscard]] static Result<VOID, Error> Decode(Span<const CHAR> input, Span<CHAR> output);

	/**
	 * @brief Calculates required output buffer size for encoding
	 *
	 * @details Computes the encoded size per RFC 4648 Section 4: each 3-byte input
	 * group produces 4 Base64 characters. The input length is rounded up to the
	 * next multiple of 3 before applying the 4/3 expansion, ensuring space for
	 * padding characters. One additional byte is reserved for the null terminator.
	 *
	 * Formula: ((inputSize + 2) / 3) * 4 + 1
	 *
	 * @param inputSize Size of input data in bytes
	 * @return Required output buffer size in bytes (including null terminator)
	 *
	 * @see RFC 4648 Section 4 — Base 64 Encoding (output length calculation)
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
	 */
	static constexpr UINT32 GetEncodeOutSize(UINT32 inputSize)
	{
		return (UINT32)((((inputSize) + 2) / 3) * 4 + 1);
	}

	/**
	 * @brief Calculates required output buffer size for decoding
	 *
	 * @details Computes the maximum decoded size per RFC 4648 Section 4: each
	 * 4-character Base64 group decodes to 3 bytes. The actual decoded size may
	 * be 1–2 bytes fewer depending on padding characters in the input.
	 *
	 * Formula: (inputSize / 4) * 3
	 *
	 * @param inputSize Size of Base64 input string in bytes (should be a multiple of 4)
	 * @return Maximum output buffer size in bytes (actual decoded size may be smaller due to padding)
	 *
	 * @see RFC 4648 Section 4 — Base 64 Encoding (decoded length calculation)
	 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
	 */
	static constexpr UINT32 GetDecodeOutSize(UINT32 inputSize)
	{
		return (UINT32)(((inputSize) >> 2) * 3);
	}
};

/** @} */ // end of base64 group
