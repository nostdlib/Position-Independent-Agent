/**
 * @file utf16.h
 * @brief UTF-16 to UTF-8 Encoding Conversion
 *
 * @details Provides platform-independent UTF-16 to UTF-8 conversion utilities.
 * Essential for handling Windows wide strings (WCHAR) in network protocols
 * and text processing that requires UTF-8 encoding.
 *
 * Features:
 * - UTF-16 surrogate pair handling for characters beyond BMP (U+10000–U+10FFFF)
 * - Single codepoint conversion
 * - Full string conversion
 * - No CRT dependencies
 *
 * @see RFC 3629 — UTF-8, a transformation format of ISO 10646
 *      https://datatracker.ietf.org/doc/html/rfc3629
 * @see RFC 2781 — UTF-16, an encoding of ISO 10646
 *      https://datatracker.ietf.org/doc/html/rfc2781
 * @see The Unicode Standard — https://www.unicode.org/versions/latest/
 *
 * @note Windows uses UTF-16LE (Little Endian) for wide strings (WCHAR).
 *
 * @ingroup core
 *
 * @defgroup utf16 UTF-16 Encoding
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"

/**
 * @class UTF16
 * @brief Static class for UTF-16 to UTF-8 conversion operations
 *
 * @details Provides position-independent UTF-16 to UTF-8 conversion without
 * CRT dependencies. Properly handles UTF-16 surrogate pairs for characters
 * outside the Basic Multilingual Plane (BMP).
 *
 * @par Unicode Encoding Overview:
 * - UTF-16: Each code unit is 2 bytes. Characters outside BMP use surrogate pairs.
 * - UTF-8: Variable length (1-4 bytes per character). ASCII compatible.
 *
 * @par Example Usage:
 * @code
 * auto wide = L"Hello, World!";
 * CHAR utf8[64];
 * USIZE len = UTF16::ToUTF8(Span<const WCHAR>(wide, 13), Span<CHAR>(utf8, sizeof(utf8)));
 * utf8[len] = '\0';  // Null-terminate
 * @endcode
 */
class UTF16
{
public:
	/**
	 * @brief Convert a single UTF-16 code unit (or surrogate pair) to UTF-8
	 * @param input UTF-16 input span
	 * @param inputIndex Current index into input (advanced past consumed code units on return)
	 * @param output Buffer to receive UTF-8 bytes (must be at least 4 bytes)
	 * @return Number of UTF-8 bytes written (1-4), or 0 if no input available
	 *
	 * @details Processes one Unicode codepoint from the UTF-16 input. For characters
	 * in the BMP (U+0000 to U+FFFF), consumes one code unit. For characters
	 * outside the BMP (U+10000 to U+10FFFF), consumes a surrogate pair
	 * (high surrogate 0xD800-0xDBFF followed by low surrogate 0xDC00-0xDFFF).
	 *
	 * @note The inputIndex parameter is advanced past the consumed code unit(s).
	 *
	 * @see RFC 2781 Section 2.1 — Encoding UTF-16 (surrogate pair algorithm)
	 *      https://datatracker.ietf.org/doc/html/rfc2781#section-2.1
	 */
	static constexpr USIZE CodepointToUTF8(Span<const WCHAR> input, USIZE& inputIndex, Span<CHAR> output)
	{
		if (inputIndex >= input.Size())
			return 0;

		UINT32 codepoint = input[inputIndex++];

		// Handle UTF-16 surrogate pairs
		if (codepoint >= 0xD800 && codepoint <= 0xDBFF && inputIndex < input.Size())
		{
			UINT32 low = input[inputIndex];
			if (low >= 0xDC00 && low <= 0xDFFF)
			{
				codepoint = 0x10000 + ((codepoint & 0x3FF) << 10) + (low & 0x3FF);
				inputIndex++;
			}
		}

		return CodepointToUTF8Bytes(codepoint, output);
	}

	/**
	 * @brief Convert a Unicode codepoint to UTF-8 bytes
	 * @param codepoint Unicode codepoint (U+0000 to U+10FFFF)
	 * @param output Buffer to receive UTF-8 bytes (must be at least 4 bytes)
	 * @return Number of UTF-8 bytes written (1-4), or 0 for invalid codepoint
	 *
	 * @details UTF-8 encoding scheme (RFC 3629 Section 3):
	 * - U+0000 to U+007F: 1 byte  (0xxxxxxx)
	 * - U+0080 to U+07FF: 2 bytes (110xxxxx 10xxxxxx)
	 * - U+0800 to U+FFFF: 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
	 * - U+10000 to U+10FFFF: 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
	 *
	 * @see RFC 3629 Section 3 — UTF-8 definition
	 *      https://datatracker.ietf.org/doc/html/rfc3629#section-3
	 */
	static constexpr USIZE CodepointToUTF8Bytes(UINT32 codepoint, Span<CHAR> output)
	{
		if (codepoint < 0x80)
		{
			// 1-byte sequence (ASCII)
			if (output.Size() < 1)
				return 0;
			output[0] = (CHAR)codepoint;
			return 1;
		}
		else if (codepoint < 0x800)
		{
			// 2-byte sequence
			if (output.Size() < 2)
				return 0;
			output[0] = (CHAR)(0xC0 | (codepoint >> 6));
			output[1] = (CHAR)(0x80 | (codepoint & 0x3F));
			return 2;
		}
		else if (codepoint < 0x10000)
		{
			// Reject surrogates (U+D800 to U+DFFF) per RFC 3629 Section 3
			if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
				return 0;
			// 3-byte sequence
			if (output.Size() < 3)
				return 0;
			output[0] = (CHAR)(0xE0 | (codepoint >> 12));
			output[1] = (CHAR)(0x80 | ((codepoint >> 6) & 0x3F));
			output[2] = (CHAR)(0x80 | (codepoint & 0x3F));
			return 3;
		}
		else if (codepoint < 0x110000)
		{
			// 4-byte sequence
			if (output.Size() < 4)
				return 0;
			output[0] = (CHAR)(0xF0 | (codepoint >> 18));
			output[1] = (CHAR)(0x80 | ((codepoint >> 12) & 0x3F));
			output[2] = (CHAR)(0x80 | ((codepoint >> 6) & 0x3F));
			output[3] = (CHAR)(0x80 | (codepoint & 0x3F));
			return 4;
		}

		// Invalid codepoint
		return 0;
	}

	/**
	 * @brief Convert UTF-16 string to UTF-8
	 * @param input UTF-16 input span
	 * @param output Buffer to receive UTF-8 output
	 * @return Total number of UTF-8 bytes written (excluding null terminator)
	 *
	 * @details Converts an entire UTF-16 string to UTF-8. Properly handles
	 * surrogate pairs for characters outside the Basic Multilingual Plane.
	 *
	 * @note Output buffer should be at least inputLength * 4 bytes to guarantee
	 * no truncation (worst case: all 4-byte UTF-8 sequences).
	 *
	 * @warning Does not null-terminate the output. Caller must add null terminator
	 * if needed.
	 *
	 * @see RFC 3629 — UTF-8, a transformation format of ISO 10646
	 *      https://datatracker.ietf.org/doc/html/rfc3629
	 * @see RFC 2781 — UTF-16, an encoding of ISO 10646
	 *      https://datatracker.ietf.org/doc/html/rfc2781
	 */
	static constexpr USIZE ToUTF8(Span<const WCHAR> input, Span<CHAR> output)
	{
		USIZE inputIndex = 0;
		USIZE outputIndex = 0;

		while (inputIndex < input.Size() && outputIndex + 4 <= output.Size())
		{
			USIZE bytesWritten = CodepointToUTF8(input, inputIndex, output.Subspan(outputIndex));
			outputIndex += bytesWritten;
		}

		return outputIndex;
	}

	/**
	 * @brief Convert a UTF-16 string to UTF-8, reversing surrogate escapes
	 * @param input UTF-16 input span (may contain U+DC80..U+DCFF escape units)
	 * @param output Buffer to receive UTF-8 output
	 * @return Total number of UTF-8 bytes written (excluding null terminator)
	 *
	 * @details Identical to ToUTF8 for well-formed input, except that a LONE
	 * low surrogate in U+DC80..U+DCFF — the surrogateescape encoding produced
	 * by StringUtils::Utf8ToWideLossless for undecodable filename bytes (each
	 * escaped byte b >= 0x80 maps to and from U+DC00 + b) — is
	 * converted back to the single raw byte it stands for instead of being
	 * rejected. A low surrogate in that window is treated as an escape only
	 * when it is NOT the second half of a valid surrogate pair: pairs are
	 * consumed atomically by CodepointToUTF8, so a unit reached at the top of
	 * the loop is necessarily unpaired. This makes the UTF-8 → wide → UTF-16
	 * → wide → UTF-8 round trip byte-exact for arbitrary filenames.
	 *
	 * @warning Does not null-terminate the output. Caller must add null
	 * terminator if needed.
	 *
	 * @see StringUtils::Utf8ToWideLossless — the encoding half of the contract
	 */
	static constexpr USIZE ToUTF8Lossless(Span<const WCHAR> input, Span<CHAR> output)
	{
		USIZE inputIndex = 0;
		USIZE outputIndex = 0;

		while (inputIndex < input.Size() && outputIndex + 4 <= output.Size())
		{
			WCHAR unit = input[inputIndex];

			// Valid pair (any high surrogate followed by any low surrogate):
			// consume both units through the normal path so an astral
			// codepoint whose low half lands inside the escape window —
			// U+1E400..U+1FFFF and friends — is encoded, not unescaped.
			BOOL isPair = unit >= 0xD800 && unit <= 0xDBFF
						  && inputIndex + 1 < input.Size()
						  && input[inputIndex + 1] >= 0xDC00 && input[inputIndex + 1] <= 0xDFFF;

			if (!isPair && unit >= 0xDC80 && unit <= 0xDCFF)
			{
				output[outputIndex++] = (CHAR)(unit & 0xFF);
				inputIndex++;
				continue;
			}

			USIZE bytesWritten = CodepointToUTF8(input, inputIndex, output.Subspan(outputIndex));
			outputIndex += bytesWritten;

			// CodepointToUTF8 always consumes at least one input unit, so an
			// unpaired surrogate outside the escape window (rejected by
			// CodepointToUTF8Bytes, zero bytes written) still advances the
			// loop — dropped, exactly like ToUTF8 drops corrupt input.
		}

		return outputIndex;
	}
};

/** @} */ // end of utf16 group
