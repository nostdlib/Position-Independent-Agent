#include "core/algorithms/base64.h"

/// Branchless mapping from a 6-bit value (0–63) to its Base64 alphabet character.
/// Uses arithmetic offsets to avoid a lookup table in .rdata.
///
/// The standard Base64 alphabet (RFC 4648 Section 4, Table 1):
///   0–25  → 'A'–'Z'  (offset: +'A')
///   26–51 → 'a'–'z'  (offset: +'a' - 26)
///   52–61 → '0'–'9'  (offset: +'0' - 52)
///   62    → '+'
///   63    → '/'
///
/// @see RFC 4648 Section 4 — Base 64 Encoding (Table 1: The Base 64 Alphabet)
///      https://datatracker.ietf.org/doc/html/rfc4648#section-4
static constexpr CHAR Base64EncodeChar(UINT32 v)
{
	v &= 63u;
	return (CHAR)(
		'A' + v +
		(v >= 26u) * ('a' - 'A' - 26) +
		(v >= 52u) * ('0' - 'a' - 26) +
		(v >= 62u) * ('+' - '0' - 10) +
		(v >= 63u) * ('/' - '+' - 1));
}

/// Maps a Base64 alphabet character to its 6-bit value (0–63).
/// Returns 0xFF for any character not in the alphabet, enabling
/// callers to detect invalid input.
///
/// @see RFC 4648 Section 4 — Base 64 Encoding (Table 1: The Base 64 Alphabet)
///      https://datatracker.ietf.org/doc/html/rfc4648#section-4
static constexpr UINT32 Base64DecodeChar(CHAR c)
{
	if (c >= 'A' && c <= 'Z') return (UINT32)(c - 'A');
	if (c >= 'a' && c <= 'z') return (UINT32)(c - 'a' + 26);
	if (c >= '0' && c <= '9') return (UINT32)(c - '0' + 52);
	if (c == '+') return 62u;
	if (c == '/') return 63u;
	return 0xFFu;
}

/**
 * @brief Encodes binary data to Base64 format
 *
 * @details Implements the encoding procedure from RFC 4648 Section 4:
 *
 * 1. Input bytes are consumed in 3-byte (24-bit) groups
 * 2. Each 24-bit group is split into four 6-bit values
 * 3. Each 6-bit value is mapped to the Base64 alphabet via Base64EncodeChar
 * 4. When fewer than 3 bytes remain (RFC 4648 Section 3.5):
 *    - 1 byte  → 2 alphabet chars + "==" (8 bits padded to 12, 2 zero bits discarded)
 *    - 2 bytes → 3 alphabet chars + "="  (16 bits padded to 18, 2 zero bits discarded)
 * 5. Output is null-terminated
 *
 * @see RFC 4648 Section 4 — Base 64 Encoding
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
 * @see RFC 4648 Section 3.5 — Canonical Encoding (padding rules)
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.5
 */
VOID Base64::Encode(Span<const CHAR> input, Span<CHAR> output)
{
	const UINT32 inputSize = (UINT32)input.Size();
	const UINT32 outputSize = (UINT32)output.Size();
	UINT32 i = 0;
	UINT32 o = 0;

	/* Process full 3-byte (24-bit) groups — RFC 4648 Section 4 */
	while (i + 2u < inputSize && o + 4u < outputSize)
	{
		UINT32 v =
			((UINT32)(UINT8)input[i] << 16) |
			((UINT32)(UINT8)input[i + 1] << 8) |
			((UINT32)(UINT8)input[i + 2]);

		output[o++] = Base64EncodeChar((v >> 18) & 63u);
		output[o++] = Base64EncodeChar((v >> 12) & 63u);
		output[o++] = Base64EncodeChar((v >> 6) & 63u);
		output[o++] = Base64EncodeChar(v & 63u);

		i += 3u;
	}

	/* Tail padding — RFC 4648 Section 3.5 */
	if (i < inputSize && o + 4u < outputSize)
	{
		UINT32 v = ((UINT32)(UINT8)input[i] << 16);

		output[o++] = Base64EncodeChar((v >> 18) & 63u);

		if (i + 1u < inputSize)
		{
			/* 2 remaining bytes → 3 encoded chars + 1 pad */
			v |= ((UINT32)(UINT8)input[i + 1] << 8);

			output[o++] = Base64EncodeChar((v >> 12) & 63u);
			output[o++] = Base64EncodeChar((v >> 6) & 63u);
			output[o++] = '=';
		}
		else
		{
			/* 1 remaining byte → 2 encoded chars + 2 pads */
			output[o++] = Base64EncodeChar((v >> 12) & 63u);
			output[o++] = '=';
			output[o++] = '=';
		}
	}

	if (o < outputSize)
		output[o] = 0;
}

/**
 * @brief Decodes Base64 formatted data back to binary
 *
 * @details Implements the decoding procedure from RFC 4648 Section 4:
 *
 * 1. Input is consumed in 4-character blocks
 * 2. Each character is reverse-mapped to its 6-bit value via Base64DecodeChar
 * 3. The four 6-bit values are combined into a 24-bit group
 * 4. The 24-bit group is split into 3 output bytes
 * 5. Padding characters ('=') in the last block reduce output:
 *    - "xx==" → 1 output byte  (RFC 4648 Section 3.5)
 *    - "xxx=" → 2 output bytes (RFC 4648 Section 3.5)
 *
 * Returns Base64_DecodeFailed when the input length is not a multiple of 4,
 * contains characters outside the Base64 alphabet, or has invalid padding.
 *
 * @see RFC 4648 Section 4 — Base 64 Encoding (decoding procedure)
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-4
 * @see RFC 4648 Section 3.3 — Interpretation of Non-Alphabet Characters
 *      https://datatracker.ietf.org/doc/html/rfc4648#section-3.3
 */
Result<VOID, Error> Base64::Decode(Span<const CHAR> input, Span<CHAR> output)
{
	const UINT32 inputSize = (UINT32)input.Size();
	const UINT32 outputSize = (UINT32)output.Size();

	/* Input must be a multiple of 4 characters — RFC 4648 Section 4 */
	if (inputSize % 4u != 0)
		return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

	UINT32 i = 0;
	UINT32 o = 0;

	/* Process 4-character blocks — RFC 4648 Section 4 */
	while (i + 3u < inputSize)
	{
		CHAR c0 = input[i];
		CHAR c1 = input[i + 1];
		CHAR c2 = input[i + 2];
		CHAR c3 = input[i + 3];

		/* Positions 0 and 1 must never be padding */
		if (c0 == '=' || c1 == '=')
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		UINT32 a = Base64DecodeChar(c0);
		UINT32 b = Base64DecodeChar(c1);

		if (a > 63u || b > 63u)
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		BOOL pad2 = (c2 == '=');
		BOOL pad3 = (c3 == '=');

		/* If position 2 is padded, position 3 must also be padded */
		if (pad2 && !pad3)
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		/* Padding must only appear in the last block */
		if ((pad2 || pad3) && i + 4u < inputSize)
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		UINT32 c = pad2 ? 0u : Base64DecodeChar(c2);
		UINT32 d = pad3 ? 0u : Base64DecodeChar(c3);

		if ((!pad2 && c > 63u) || (!pad3 && d > 63u))
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		/* Reassemble 24-bit group from four 6-bit values */
		UINT32 v = (a << 18) | (b << 12) | (c << 6) | d;

		if (o + 3u > outputSize)
			return Result<VOID, Error>::Err(Error::Base64_DecodeFailed);

		output[o++] = (CHAR)((v >> 16) & 0xFF);

		/* Padding check — fewer output bytes when '=' is present (RFC 4648 Section 3.5) */
		if (!pad2)
			output[o++] = (CHAR)((v >> 8) & 0xFF);

		if (!pad3)
			output[o++] = (CHAR)(v & 0xFF);

		i += 4u;
	}

	return Result<VOID, Error>::Ok();
}
