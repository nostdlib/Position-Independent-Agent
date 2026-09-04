/**
 * @file string.h
 * @brief String Manipulation Utilities
 *
 * @details Provides comprehensive string manipulation functions without CRT dependencies.
 * All operations are position-independent and work with both narrow (CHAR) and wide (WCHAR)
 * character types through template specialization.
 *
 * Features:
 * - Character classification (IsSpace, IsDigit, IsAlpha, IsAlphaNum)
 * - Case conversion (ToLowerCase, ToUpperCase)
 * - String comparison and searching
 * - String copying and manipulation
 * - Number to string conversion
 * - String to number parsing
 * - UTF-8 to UTF-16 conversion
 *
 * @note All functions are designed for position-independent code with no .rdata dependencies.
 *
 * @ingroup core
 *
 * @defgroup string String Utilities
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/types/error.h"
#include "core/types/result.h"

/**
 * @class StringUtils
 * @brief Static class providing string manipulation utilities
 *
 * @details Provides a comprehensive set of string operations without CRT dependencies.
 * All methods are template-based where appropriate, supporting both CHAR and WCHAR types.
 * Methods are force-inlined for performance.
 *
 * @par Example Usage:
 * @code
 * // Character classification
 * BOOL isDigit = StringUtils::IsDigit('5');           // true
 * BOOL isSpace = StringUtils::IsSpace('\t');          // true
 *
 * // String operations
 * USIZE len = StringUtils::Length("Hello");           // 5
 * BOOL eq = StringUtils::Equals("foo", "foo");        // true
 *
 * // Number conversion
 * CHAR buf[32];
 * StringUtils::IntToStr(-42, Span<CHAR>(buf));        // "-42"
 * INT64 num = StringUtils::ParseInt64("12345");       // 12345
 * @endcode
 */
class StringUtils
{
public:
	/// @name Character Classification
	/// @{

	/**
	 * @brief Check if character is whitespace
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to check
	 * @return true if whitespace (space, tab, newline, etc.), false otherwise
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE BOOL IsSpace(TChar c) noexcept;

	/**
	 * @brief Check if character is a decimal digit (0-9)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to check
	 * @return true if digit, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE BOOL IsDigit(TChar c) noexcept;

	/**
	 * @brief Check if character is alphabetic (a-z, A-Z)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to check
	 * @return true if alphabetic, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE BOOL IsAlpha(TChar c) noexcept;

	/**
	 * @brief Check if character is alphanumeric (a-z, A-Z, 0-9)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to check
	 * @return true if alphanumeric, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE BOOL IsAlphaNum(TChar c) noexcept;

	/// @}
	/// @name Character Conversion
	/// @{

	/**
	 * @brief Convert character to lowercase
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to convert
	 * @return Lowercase character, or original if not uppercase
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE TChar ToLowerCase(TChar c) noexcept;

	/**
	 * @brief Convert character to uppercase
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param c Character to convert
	 * @return Uppercase character, or original if not lowercase
	 */
	template <TCHAR TChar>
	static constexpr FORCE_INLINE TChar ToUpperCase(TChar c) noexcept;

	/// @}
	/// @name String Length and Comparison
	/// @{

	/**
	 * @brief Get length of null-terminated string
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param pChar Pointer to null-terminated string
	 * @return Number of characters (excluding null terminator)
	 */
	template <TCHAR TChar>
	static constexpr USIZE Length(const TChar *pChar) noexcept;

	/**
	 * @brief Compare two null-terminated strings
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param s1 First string
	 * @param s2 Second string
	 * @param ignoreCase If true, comparison is case-insensitive
	 * @return true if strings are equal, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL Compare(const TChar *s1, const TChar *s2, BOOL ignoreCase = false) noexcept;

	/**
	 * @brief Compare two strings with explicit lengths
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param s1 First string span
	 * @param s2 Second string span
	 * @param ignoreCase If true, comparison is case-insensitive
	 * @return true if strings are equal, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL Compare(Span<const TChar> s1, Span<const TChar> s2, BOOL ignoreCase = false) noexcept;

	/**
	 * @brief Compare two strings with explicit lengths
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param a First string span
	 * @param b Second string span
	 * @return true if strings are equal, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL Equals(Span<const TChar> a, Span<const TChar> b) noexcept;

	/**
	 * @brief Compare two null-terminated strings for equality
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param a First string
	 * @param b Second string
	 * @return true if strings are equal, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL Equals(const TChar *a, const TChar *b) noexcept;

	/**
	 * @brief Check if string starts with a prefix (null-terminated)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param pChar String to check
	 * @param pSubString Prefix to look for
	 * @return true if string starts with prefix, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL StartsWith(const TChar *pChar, const TChar *pSubString) noexcept;

	/**
	 * @brief Check if string starts with prefix (with explicit lengths)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to check
	 * @param prefix Prefix span to look for
	 * @return true if string starts with prefix, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL StartsWith(Span<const TChar> str, Span<const TChar> prefix) noexcept;

	/**
	 * @brief Check if string ends with suffix
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to check
	 * @param suffix Suffix span to look for
	 * @return true if string ends with suffix, false otherwise
	 */
	template <TCHAR TChar>
	static constexpr BOOL EndsWith(Span<const TChar> str, Span<const TChar> suffix) noexcept;

	/// @}
	/// @name String Search
	/// @{

	/**
	 * @brief Find index of character in string
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to search
	 * @param ch Character to find
	 * @return Index of first occurrence, or -1 if not found
	 */
	template <TCHAR TChar>
	static constexpr SSIZE IndexOfChar(Span<const TChar> str, TChar ch) noexcept;

	/**
	 * @brief Find index of substring in string
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to search
	 * @param sub Substring span to find
	 * @return Index of first occurrence, or -1 if not found
	 */
	template <TCHAR TChar>
	static constexpr SSIZE IndexOf(Span<const TChar> str, Span<const TChar> sub) noexcept;

	/// @}
	/// @name String Copy Operations
	/// @{

	/**
	 * @brief Safe string copy with explicit buffer size
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param dest Destination buffer span
	 * @param src Source string span
	 * @return Number of characters copied (excluding null terminator)
	 */
	template <TCHAR TChar>
	static constexpr USIZE Copy(Span<TChar> dest, Span<const TChar> src) noexcept;

	/**
	 * @brief Safe string copy with compile-time buffer size
	 * @tparam MaxLen Buffer size (deduced from array)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param dest Destination array
	 * @param src Source string span
	 * @return Number of characters copied (excluding null terminator)
	 */
	template <USIZE MaxLen, TCHAR TChar>
	static constexpr FORCE_INLINE USIZE Copy(TChar (&dest)[MaxLen], Span<const TChar> src) noexcept;

	/// @}
	/// @name String Manipulation
	/// @{

	/**
	 * @brief Trim whitespace from end of string (in-place)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str Mutable string span to trim (trailing whitespace replaced with null)
	 * @return Subspan of str with trailing whitespace excluded
	 */
	template <TCHAR TChar>
	static constexpr Span<TChar> TrimEnd(Span<TChar> str) noexcept;

	/**
	 * @brief Trim whitespace from end (read-only)
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to trim
	 * @return Subspan with trailing whitespace excluded
	 */
	template <TCHAR TChar>
	static constexpr Span<const TChar> TrimEnd(Span<const TChar> str) noexcept;

	/**
	 * @brief Trim whitespace from start
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to trim
	 * @return Subspan with leading whitespace excluded
	 */
	template <TCHAR TChar>
	static constexpr Span<const TChar> TrimStart(Span<const TChar> str) noexcept;

	/**
	 * @brief Trim whitespace from both ends
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param str String span to trim
	 * @return Subspan with leading and trailing whitespace excluded
	 */
	template <TCHAR TChar>
	static constexpr Span<const TChar> Trim(Span<const TChar> str) noexcept;

	/**
	 * @brief Concatenate two strings into a buffer
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @param buffer Destination buffer span
	 * @param s1 First string span
	 * @param s2 Second string span
	 * @return Total number of characters written (excluding null terminator)
	 */
	template <TCHAR TChar>
	static constexpr USIZE Concat(Span<TChar> buffer,
								  Span<const TChar> s1,
								  Span<const TChar> s2) noexcept;

	/// @}
	/// @name Number Conversion
	/// @{

	/**
	 * @brief Convert signed integer to string
	 * @param value Integer value to convert
	 * @param buffer Destination buffer span
	 * @return Number of characters written (excluding null terminator)
	 */
	static constexpr USIZE IntToStr(INT64 value, Span<CHAR> buffer) noexcept;

	/**
	 * @brief Convert unsigned integer to string
	 * @param value Unsigned integer value to convert
	 * @param buffer Destination buffer span
	 * @return Number of characters written (excluding null terminator)
	 */
	static constexpr USIZE UIntToStr(UINT64 value, Span<CHAR> buffer) noexcept;

	/**
	 * @brief Parse hexadecimal string to UINT32
	 * @param str Hexadecimal string span (without 0x prefix)
	 * @return Parsed value (stops at first non-hex character)
	 */
	static constexpr UINT32 ParseHex(Span<const CHAR> str) noexcept;

	/**
	 * @brief Write decimal number to buffer
	 * @param buffer Destination buffer span
	 * @param num Number to write
	 * @return Number of characters written (excluding null terminator)
	 */
	static constexpr USIZE WriteDecimal(Span<CHAR> buffer, UINT32 num) noexcept;

	/**
	 * @brief Write hexadecimal number to buffer
	 * @param buffer Destination buffer span
	 * @param num Number to write
	 * @param uppercase true for A-F, false for a-f
	 * @return Number of characters written (excluding null terminator)
	 */
	static constexpr USIZE WriteHex(Span<CHAR> buffer, UINT32 num, BOOL uppercase = false) noexcept;

	/**
	 * @brief Convert double to string
	 * @param value Floating-point value to convert
	 * @param buffer Destination buffer span
	 * @param precision Number of decimal places (default 6)
	 * @return Number of characters written (excluding null terminator)
	 */
	static USIZE FloatToStr(double value, Span<CHAR> buffer, UINT8 precision = 6) noexcept;

	/**
	 * @brief Parse string to INT64 (with explicit length)
	 * @param str String span to parse
	 * @return Result containing parsed INT64 value, or Error::String_ParseIntFailed
	 */
	[[nodiscard]] static constexpr Result<INT64, Error> ParseInt64(Span<const CHAR> str) noexcept;

	/**
	 * @brief Parse null-terminated string to INT64
	 * @param str Null-terminated string to parse
	 * @return Result containing parsed INT64 value, or Error::String_ParseIntFailed
	 */
	[[nodiscard]] static constexpr Result<INT64, Error> ParseInt64(PCCHAR str) noexcept;

	/**
	 * @brief Convert string to double
	 * @param str String span to parse
	 * @return Result containing parsed double value, or Error::String_ParseFloatFailed
	 */
	[[nodiscard]] static Result<double, Error> StrToFloat(Span<const CHAR> str) noexcept;

	/// @}
	/// @name UTF Conversion
	/// @{

	/**
	 * @brief Convert UTF-8 string to wide string
	 * @param utf8 Source UTF-8 string span
	 * @param wide Destination wide string buffer span
	 * @return Number of wide characters written (excluding null terminator)
	 *
	 * @details Decodes UTF-8 multibyte sequences (1-4 bytes per codepoint).
	 * On platforms where WCHAR is 2 bytes (Windows/UEFI), codepoints above
	 * U+FFFF are encoded as UTF-16 surrogate pairs. On platforms where WCHAR
	 * is 4 bytes (Linux/macOS), codepoints are stored directly.
	 */
	static USIZE Utf8ToWide(Span<const CHAR> utf8, Span<WCHAR> wide);

	/**
	 * @brief Convert UTF-8 string to wide string, never dropping a byte
	 * @param utf8 Source UTF-8 string span
	 * @param wide Destination wide string buffer span
	 * @return Number of wide characters written (excluding null terminator)
	 *
	 * @details Unlike Utf8ToWide — which silently skips invalid UTF-8 bytes,
	 * corrupting on-disk filenames — this variant maps every byte that is not
	 * part of a well-formed UTF-8 sequence (RFC 3629 Section 3: invalid leads,
	 * missing/invalid continuations, overlong encodings, CESU-8 surrogates,
	 * codepoints above U+10FFFF) to the lone low surrogate 0xDC00 + byte —
	 * bytes 0x80..0xFF map onto U+DC80..U+DCFF (Python "surrogateescape"). The
	 * mapping is bijective over that range, so
	 * UTF16::ToUTF8Lossless() reverses it exactly and the original filename
	 * bytes can be rebuilt byte-for-byte when the path is handed back to the OS.
	 * Valid UTF-8 (including astral codepoints) converts identically to
	 * Utf8ToWide.
	 *
	 * @see UTF16::ToUTF8Lossless — the inverse for surrogate-escaped paths
	 */
	static USIZE Utf8ToWideLossless(Span<const CHAR> utf8, Span<WCHAR> wide);

	/**
	 * @brief Convert CHAR16 (wire-format UTF-16) string to native WCHAR string
	 * @param src Source CHAR16 span (UTF-16LE from wire protocol)
	 * @param dst Destination WCHAR buffer span
	 * @return Number of WCHAR characters written (excluding null terminator)
	 *
	 * @details On Windows/UEFI where WCHAR is 2 bytes, this is a direct copy.
	 * On Linux/macOS where WCHAR is 4 bytes, surrogate pairs are decoded into
	 * full codepoints.
	 */
	static constexpr USIZE Char16ToWide(Span<const CHAR16> src, Span<WCHAR> dst) noexcept;

	/**
	 * @brief Convert native WCHAR string to CHAR16 (wire-format UTF-16)
	 * @param src Source WCHAR span
	 * @param dst Destination CHAR16 buffer span
	 * @return Number of CHAR16 code units written (excluding null terminator)
	 *
	 * @details On Windows/UEFI where WCHAR is 2 bytes, this is a direct copy.
	 * On Linux/macOS where WCHAR is 4 bytes, codepoints above U+FFFF are
	 * encoded as UTF-16 surrogate pairs.
	 */
	static constexpr USIZE WideToChar16(Span<const WCHAR> src, Span<CHAR16> dst) noexcept;

	/// @}
};

// ============================================================================
// CHARACTER CLASSIFICATION IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr FORCE_INLINE BOOL StringUtils::IsSpace(TChar c) noexcept
{
	return (c == (TChar)' ' ||	// space
			c == (TChar)'\t' || // horizontal tab
			c == (TChar)'\n' || // newline
			c == (TChar)'\v' || // vertical tab
			c == (TChar)'\f' || // form feed
			c == (TChar)'\r');	// carriage return
}

template <TCHAR TChar>
constexpr FORCE_INLINE BOOL StringUtils::IsDigit(TChar c) noexcept
{
	return (c >= (TChar)'0' && c <= (TChar)'9');
}

template <TCHAR TChar>
constexpr FORCE_INLINE BOOL StringUtils::IsAlpha(TChar c) noexcept
{
	return (c >= (TChar)'a' && c <= (TChar)'z') ||
		   (c >= (TChar)'A' && c <= (TChar)'Z');
}

template <TCHAR TChar>
constexpr FORCE_INLINE BOOL StringUtils::IsAlphaNum(TChar c) noexcept
{
	return IsAlpha(c) || IsDigit(c);
}

// ============================================================================
// CHARACTER CONVERSION IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr FORCE_INLINE TChar StringUtils::ToLowerCase(TChar c) noexcept
{
	if (c >= (TChar)'A' && c <= (TChar)'Z')
	{
		return c + ((TChar)'a' - (TChar)'A');
	}
	return c;
}

template <TCHAR TChar>
constexpr FORCE_INLINE TChar StringUtils::ToUpperCase(TChar c) noexcept
{
	if (c >= (TChar)'a' && c <= (TChar)'z')
	{
		return c - ((TChar)'a' - (TChar)'A');
	}
	return c;
}

// ============================================================================
// STRING LENGTH AND COMPARISON IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr USIZE StringUtils::Length(const TChar *p) noexcept
{
	if (!p)
		return 0;
	USIZE i = 0;
	while (p[i] != (TChar)'\0')
	{
		i++;
	}
	return i;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::Compare(const TChar *s1, const TChar *s2, BOOL ignoreCase) noexcept
{
	if (!s1 || !s2)
		return s1 == s2;
	const TChar *str1 = s1;
	const TChar *str2 = s2;
	while (*str1 && *str2)
	{
		TChar c1 = ignoreCase ? ToLowerCase(*str1) : *str1;
		TChar c2 = ignoreCase ? ToLowerCase(*str2) : *str2;
		if (c1 != c2)
		{
			return false;
		}
		str1++;
		str2++;
	}
	return (*str1 == *str2);
}

template <TCHAR TChar>
constexpr BOOL StringUtils::Compare(Span<const TChar> s1, Span<const TChar> s2, BOOL ignoreCase) noexcept
{
	if (s1.Size() != s2.Size())
		return false;
	for (USIZE i = 0; i < s1.Size(); i++)
	{
		TChar c1 = ignoreCase ? ToLowerCase(s1[i]) : s1[i];
		TChar c2 = ignoreCase ? ToLowerCase(s2[i]) : s2[i];
		if (c1 != c2)
			return false;
	}
	return true;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::Equals(Span<const TChar> a, Span<const TChar> b) noexcept
{
	if (a.Size() != b.Size())
		return false;
	for (USIZE i = 0; i < a.Size(); i++)
	{
		if (a[i] != b[i])
			return false;
	}
	return true;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::Equals(const TChar *a, const TChar *b) noexcept
{
	if (!a || !b)
		return a == b;
	while (*a != (TChar)'\0' && *b != (TChar)'\0')
	{
		if (*a != *b)
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::StartsWith(const TChar *pChar, const TChar *pSubString) noexcept
{
	USIZE i = 0;
	while (pChar[i] != (TChar)'\0' && pSubString[i] != (TChar)'\0')
	{
		if (pChar[i] != pSubString[i])
		{
			return false;
		}
		i++;
	}
	if (pSubString[i] != (TChar)'\0')
	{
		return false;
	}
	return true;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::StartsWith(Span<const TChar> str, Span<const TChar> prefix) noexcept
{
	if (prefix.Size() > str.Size())
		return false;
	for (USIZE i = 0; i < prefix.Size(); i++)
	{
		if (str[i] != prefix[i])
			return false;
	}
	return true;
}

template <TCHAR TChar>
constexpr BOOL StringUtils::EndsWith(Span<const TChar> str, Span<const TChar> suffix) noexcept
{
	if (suffix.Size() > str.Size())
		return false;
	USIZE offset = str.Size() - suffix.Size();
	for (USIZE i = 0; i < suffix.Size(); i++)
	{
		if (str[offset + i] != suffix[i])
			return false;
	}
	return true;
}

// ============================================================================
// STRING SEARCH IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr SSIZE StringUtils::IndexOfChar(Span<const TChar> str, TChar ch) noexcept
{
	for (USIZE i = 0; i < str.Size(); i++)
	{
		if (str[i] == ch)
			return (SSIZE)i;
	}
	return -1;
}

template <TCHAR TChar>
constexpr SSIZE StringUtils::IndexOf(Span<const TChar> str, Span<const TChar> sub) noexcept
{
	if (sub.Size() == 0)
		return 0;
	if (sub.Size() > str.Size())
		return -1;

	USIZE limit = str.Size() - sub.Size() + 1;
	for (USIZE i = 0; i < limit; i++)
	{
		BOOL match = true;
		for (USIZE j = 0; j < sub.Size() && match; j++)
		{
			if (str[i + j] != sub[j])
				match = false;
		}
		if (match)
			return (SSIZE)i;
	}
	return -1;
}

// ============================================================================
// STRING COPY IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr USIZE StringUtils::Copy(Span<TChar> dest, Span<const TChar> src) noexcept
{
	if (!dest.Data() || dest.Size() == 0)
		return 0;
	if (!src.Data() || src.Size() == 0)
	{
		dest[0] = (TChar)'\0';
		return 0;
	}

	USIZE copyLen = src.Size() < dest.Size() - 1 ? src.Size() : dest.Size() - 1;
	for (USIZE i = 0; i < copyLen; i++)
	{
		dest[i] = src[i];
	}
	dest[copyLen] = (TChar)'\0';
	return copyLen;
}

template <USIZE MaxLen, TCHAR TChar>
constexpr FORCE_INLINE USIZE StringUtils::Copy(TChar (&dest)[MaxLen], Span<const TChar> src) noexcept
{
	return Copy(Span<TChar>(dest), src);
}

// ============================================================================
// STRING MANIPULATION IMPLEMENTATIONS
// ============================================================================

template <TCHAR TChar>
constexpr Span<TChar> StringUtils::TrimEnd(Span<TChar> str) noexcept
{
	USIZE len = str.Size();
	while (len > 0 && IsSpace(str[len - 1]))
	{
		str[len - 1] = (TChar)'\0';
		len--;
	}
	return Span<TChar>(str.Data(), len);
}

template <TCHAR TChar>
constexpr Span<const TChar> StringUtils::TrimEnd(Span<const TChar> str) noexcept
{
	USIZE len = str.Size();
	while (len > 0 && IsSpace(str[len - 1]))
	{
		len--;
	}
	return Span<const TChar>(str.Data(), len);
}

template <TCHAR TChar>
constexpr Span<const TChar> StringUtils::TrimStart(Span<const TChar> str) noexcept
{
	USIZE offset = 0;
	while (offset < str.Size() && IsSpace(str[offset]))
	{
		offset++;
	}
	return str.Subspan(offset);
}

template <TCHAR TChar>
constexpr Span<const TChar> StringUtils::Trim(Span<const TChar> str) noexcept
{
	Span<const TChar> trimmed = TrimStart(str);
	return TrimEnd(trimmed);
}

template <TCHAR TChar>
constexpr USIZE StringUtils::Concat(Span<TChar> buffer,
									Span<const TChar> s1,
									Span<const TChar> s2) noexcept
{
	if (!buffer.Data() || buffer.Size() == 0)
		return 0;

	USIZE pos = 0;

	for (USIZE i = 0; i < s1.Size() && pos < buffer.Size() - 1; i++)
	{
		buffer[pos++] = s1[i];
	}

	for (USIZE i = 0; i < s2.Size() && pos < buffer.Size() - 1; i++)
	{
		buffer[pos++] = s2[i];
	}

	buffer[pos] = (TChar)'\0';
	return pos;
}

// ============================================================================
// NUMBER CONVERSION IMPLEMENTATIONS
// ============================================================================

constexpr USIZE StringUtils::IntToStr(INT64 value, Span<CHAR> buffer) noexcept
{
	if (buffer.Size() < 2)
		return 0;

	CHAR temp[24];
	USIZE pos = 0;
	BOOL negative = false;

	if (value < 0)
	{
		negative = true;
	}

	// Use unsigned to handle INT64_MIN correctly (negating INT64_MIN is UB)
	UINT64 uval = negative ? (UINT64)0 - (UINT64)value : (UINT64)value;

	if (uval == 0)
	{
		temp[pos++] = '0';
	}
	else
	{
		while (uval > 0 && pos < 22)
		{
			temp[pos++] = '0' + (CHAR)(uval % 10);
			uval = uval / 10;
		}
	}

	if (negative && pos < 22)
	{
		temp[pos++] = '-';
	}

	USIZE copyLen = pos < buffer.Size() - 1 ? pos : buffer.Size() - 1;
	for (USIZE i = 0; i < copyLen; i++)
	{
		buffer[i] = temp[pos - 1 - i];
	}
	buffer[copyLen] = '\0';
	return copyLen;
}

constexpr USIZE StringUtils::UIntToStr(UINT64 value, Span<CHAR> buffer) noexcept
{
	if (buffer.Size() < 2)
		return 0;

	CHAR temp[24];
	USIZE pos = 0;

	if (value == 0)
	{
		temp[pos++] = '0';
	}
	else
	{
		while (value > 0 && pos < 22)
		{
			temp[pos++] = '0' + (CHAR)(value % 10);
			value = value / 10;
		}
	}

	USIZE copyLen = pos < buffer.Size() - 1 ? pos : buffer.Size() - 1;
	for (USIZE i = 0; i < copyLen; i++)
	{
		buffer[i] = temp[pos - 1 - i];
	}
	buffer[copyLen] = '\0';
	return copyLen;
}

constexpr UINT32 StringUtils::ParseHex(Span<const CHAR> str) noexcept
{
	UINT32 result = 0;
	for (USIZE i = 0; i < str.Size(); i++)
	{
		CHAR c = str[i];
		UINT32 digit = 0;

		if (c >= '0' && c <= '9')
		{
			digit = c - '0';
		}
		else if (c >= 'a' && c <= 'f')
		{
			digit = 10 + (c - 'a');
		}
		else if (c >= 'A' && c <= 'F')
		{
			digit = 10 + (c - 'A');
		}
		else
		{
			break;
		}

		result = (result << 4) | digit;
	}
	return result;
}

constexpr USIZE StringUtils::WriteDecimal(Span<CHAR> buffer, UINT32 num) noexcept
{
	return UIntToStr((UINT64)num, buffer);
}

constexpr USIZE StringUtils::WriteHex(Span<CHAR> buffer, UINT32 num, BOOL uppercase) noexcept
{
	if (buffer.Size() < 2)
		return 0;

	if (num == 0)
	{
		buffer[0] = '0';
		buffer[1] = '\0';
		return 1;
	}

	CHAR temp[9];
	INT32 i = 0;
	CHAR baseChar = uppercase ? 'A' : 'a';

	while (num > 0)
	{
		UINT32 digit = num & 0xF;
		temp[i++] = (digit < 10) ? ('0' + digit) : (baseChar + digit - 10);
		num >>= 4;
	}

	INT32 j = 0;
	while (i > 0 && j < (INT32)buffer.Size() - 1)
	{
		buffer[j++] = temp[--i];
	}
	buffer[j] = '\0';
	return (USIZE)j;
}

constexpr Result<INT64, Error> StringUtils::ParseInt64(Span<const CHAR> str) noexcept
{
	if (str.Size() == 0)
	{
		return Result<INT64, Error>::Err(Error::String_ParseIntFailed);
	}

	USIZE i = 0;
	BOOL negative = false;

	while (i < str.Size() && (str[i] == ' ' || str[i] == '\t'))
	{
		i++;
	}

	if (i < str.Size() && str[i] == '-')
	{
		negative = true;
		i++;
	}
	else if (i < str.Size() && str[i] == '+')
	{
		i++;
	}

	UINT64 value = 0;
	BOOL hasDigits = false;
	constexpr UINT64 maxPositive = 0x7FFFFFFFFFFFFFFFULL;
	constexpr UINT64 maxNegative = 0x8000000000000000ULL;
	UINT64 limit = negative ? maxNegative : maxPositive;

	while (i < str.Size() && str[i] >= '0' && str[i] <= '9')
	{
		UINT64 digit = (UINT64)(str[i] - '0');
		if (value > (limit - digit) / 10)
			return Result<INT64, Error>::Err(Error::String_ParseIntFailed);
		value = value * 10 + digit;
		hasDigits = true;
		i++;
	}

	if (!hasDigits)
	{
		return Result<INT64, Error>::Err(Error::String_ParseIntFailed);
	}

	INT64 result = negative ? -(INT64)value : (INT64)value;
	return Result<INT64, Error>::Ok(result);
}

constexpr Result<INT64, Error> StringUtils::ParseInt64(PCCHAR str) noexcept
{
	if (!str)
		return Result<INT64, Error>::Err(Error::String_ParseIntFailed);
	return ParseInt64(Span<const CHAR>(str, Length(str)));
}

// ============================================================================
// CHAR16 <-> WCHAR CONVERSION IMPLEMENTATIONS
// ============================================================================

constexpr USIZE StringUtils::Char16ToWide(Span<const CHAR16> src, Span<WCHAR> dst) noexcept
{
	if (dst.Size() == 0)
		return 0;

	USIZE si = 0, di = 0;
	while (si < src.Size() && src[si] != 0 && di + 1 < dst.Size())
	{
		UINT32 ch = src[si++];

		// Decode UTF-16 surrogate pair
		if (ch >= 0xD800 && ch <= 0xDBFF && si < src.Size())
		{
			UINT32 low = src[si];
			if (low >= 0xDC00 && low <= 0xDFFF)
			{
				ch = 0x10000 + ((ch & 0x3FF) << 10) + (low & 0x3FF);
				si++;
			}
		}

		dst[di++] = (WCHAR)ch;
	}

	dst[di] = (WCHAR)0;
	return di;
}

constexpr USIZE StringUtils::WideToChar16(Span<const WCHAR> src, Span<CHAR16> dst) noexcept
{
	if (dst.Size() == 0)
		return 0;

	USIZE si = 0, di = 0;
	while (si < src.Size() && src[si] != 0 && di + 1 < dst.Size())
	{
		UINT32 ch = (UINT32)src[si++];

		if (ch >= 0x10000)
		{
			// Encode as UTF-16 surrogate pair
			if (di + 2 >= dst.Size())
				break;
			ch -= 0x10000;
			dst[di++] = (CHAR16)(0xD800 + (ch >> 10));
			dst[di++] = (CHAR16)(0xDC00 + (ch & 0x3FF));
		}
		else
		{
			dst[di++] = (CHAR16)ch;
		}
	}

	dst[di] = 0;
	return di;
}

/** @} */ // end of string group
