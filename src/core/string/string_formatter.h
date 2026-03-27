/**
 * @file string_formatter.h
 * @brief Printf-style String Formatting
 *
 * @details Provides printf-style string formatting without CRT dependencies.
 * Uses a callback-based writer interface for flexible output destinations
 * (console, buffer, file, network, etc.).
 *
 * Supported format specifiers:
 * - %d, %D - Signed integer (INT32)
 * - %u, %U - Unsigned integer (UINT32)
 * - %x, %X - Hexadecimal (lowercase/uppercase)
 * - %f, %F - Floating-point (double)
 * - %s, %S - String (CHAR*)
 * - %ws, %ls - Wide string (WCHAR*)
 * - %p, %P - Pointer
 * - %c, %C - Character
 * - %ld, %lu - Long integers
 * - %lld, %llu - Long long integers
 * - %lx, %lX, %llx, %llX - Long/long long hex
 * - %zu, %zd - Size_t variants
 * - %e, %E - Error value (Error struct from Result::Error())
 * - %% - Literal percent sign
 *
 * Format flags:
 * - Field width (e.g., %10d)
 * - Zero padding (e.g., %08x)
 * - Left alignment (e.g., %-10s)
 * - Precision for floats (e.g., %.3f)
 * - Alternate form prefix (e.g., %#x for 0x prefix)
 *
 * @note All formatting is position-independent with no .rdata dependencies.
 *
 * @ingroup core
 *
 * @defgroup formatter String Formatting
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/string/string.h"
#include "core/types/error.h"

/**
 * @class StringFormatter
 * @brief Printf-style string formatting with callback-based output
 *
 * @details Provides type-safe, variadic template-based string formatting.
 * Output is written through a callback function, enabling flexible output
 * destinations without buffer management concerns.
 *
 * @par Writer Callback:
 * The writer callback has the signature: BOOL (*writer)(PVOID context, TChar ch)
 * - Returns true to continue formatting
 * - Returns false to stop formatting (e.g., buffer full)
 *
 * @par Example Usage:
 * @code
 * // Buffer writer callback
 * struct BufferContext {
 *     CHAR* buffer;
 *     USIZE size;
 *     USIZE pos;
 * };
 *
 * BOOL BufferWriter(PVOID ctx, CHAR ch) {
 *     BufferContext* c = (BufferContext*)ctx;
 *     if (c->pos >= c->size - 1) return false;
 *     c->buffer[c->pos++] = ch;
 *     return true;
 * }
 *
 * // Format string
 * BufferContext ctx = { buffer, sizeof(buffer), 0 };
 * StringFormatter::Format(BufferWriter, &ctx, "Value: %d, Hex: %08X", 42, 0xDEADBEEF);
 * ctx.buffer[ctx.pos] = '\0';
 * @endcode
 */
class StringFormatter
{
public:
	/**
	 * @brief Type-erased argument holder for variadic formatting
	 * @details Uses PIC-safe types from primitives.h. Supports automatic
	 * type conversion from common C++ types. Public so Logger can
	 * type-erase arguments before calling FormatWithArgs directly,
	 * eliminating per-argument-type template instantiations.
	 */
	struct Argument
	{
		/** @brief Argument type enumeration */
		enum class Type
		{
			INT32,
			UINT32,
			INT64,
			UINT64,
			DOUBLE,
			CSTR,
			WSTR,
			PTR,
			ERROR_VALUE
		};

		Type Kind; ///< Type of stored value
		union
		{
			INT32 I32;         ///< Signed 32-bit integer
			UINT32 U32;        ///< Unsigned 32-bit integer
			INT64 I64;         ///< Signed 64-bit integer
			UINT64 U64;        ///< Unsigned 64-bit integer
			double Dbl;        ///< Floating-point value
			const CHAR *Cstr;  ///< Narrow string pointer
			const WCHAR *Wstr; ///< Wide string pointer
			PVOID Ptr;         ///< Generic pointer
			Error ErrValue;    ///< Single error value
		};

		/// @name Constructors
		/// @{
		Argument() : Kind(Type::INT32), I32(0) {}
		Argument(INT32 v) : Kind(Type::INT32), I32(v) {}
		Argument(UINT32 v) : Kind(Type::UINT32), U32(v) {}
		Argument(double v) : Kind(Type::DOUBLE), Dbl(v) {}
		Argument(float v) : Kind(Type::DOUBLE), Dbl(static_cast<double>(v)) {}
		Argument(const CHAR *v) : Kind(Type::CSTR), Cstr(v) {}
		Argument(CHAR *v) : Kind(Type::CSTR), Cstr(v) {}
		Argument(const WCHAR *v) : Kind(Type::WSTR), Wstr(v) {}
		Argument(WCHAR *v) : Kind(Type::WSTR), Wstr(v) {}
		Argument(PVOID v) : Kind(Type::PTR), Ptr(v) {}
		Argument(const VOID *v) : Kind(Type::PTR), Ptr(const_cast<PVOID>(v)) {}
		Argument(const Error &v) : Kind(Type::ERROR_VALUE), ErrValue(v) {}
		Argument(INT64 v) : Kind(Type::INT64), I64(v) {}
		Argument(UINT64 v) : Kind(Type::UINT64), U64(v) {}
#if defined(__LP64__) || defined(_LP64)
		Argument(signed long v) : Kind(Type::INT64), I64(INT64(v)) {}
		Argument(unsigned long v) : Kind(Type::UINT64), U64(UINT64(v)) {}
#else
		Argument(signed long v) : Kind(Type::INT32), I32(INT32(v)) {}
		Argument(unsigned long v) : Kind(Type::UINT32), U32(UINT32(v)) {}
#endif
		/// @}
	};

	/** @brief Format with pre-erased argument array (public for Logger type-erasure) */
	template <TCHAR TChar>
	static INT32 FormatWithArgs(BOOL (*writer)(PVOID, TChar), PVOID context, const TChar *format, Span<const Argument> args);

private:
	/// @name Internal Formatting Functions
	/// @{
	template <TCHAR TChar>
	static INT32 FormatInt64(BOOL (*writer)(PVOID, TChar), PVOID context, INT64 num, INT32 width = 0, INT32 zeroPad = 0, INT32 leftAlign = 0);
	template <TCHAR TChar>
	static INT32 FormatUInt64(BOOL (*writer)(PVOID, TChar), PVOID context, UINT64 num, INT32 width = 0, INT32 zeroPad = 0, INT32 leftAlign = 0, TChar signChar = 0);
	template <TCHAR TChar>
	static INT32 FormatUInt64AsHex(BOOL (*writer)(PVOID, TChar), PVOID context, UINT64 num, INT32 fieldWidth = 0, INT32 uppercase = 0, INT32 zeroPad = 0, BOOL addPrefix = false);
	template <TCHAR TChar>
	static INT32 FormatDouble(BOOL (*writer)(PVOID, TChar), PVOID context, double num, INT32 precision = 6, INT32 width = 0, INT32 zeroPad = 0);
	template <TCHAR TChar>
	static INT32 FormatPointerAsHex(BOOL (*writer)(PVOID, TChar), PVOID context, PVOID ptr);
	template <TCHAR TChar>
	static INT32 FormatWideString(BOOL (*writer)(PVOID, TChar), PVOID context, const WCHAR *wstr, INT32 fieldWidth = 0, INT32 leftAlign = 0);
	template <TCHAR TChar>
	static INT32 FormatErrorEntry(BOOL (*writer)(PVOID, TChar), PVOID context, UINT32 code, Error::PlatformKind platform);
	template <TCHAR TChar>
	static INT32 FormatError(BOOL (*writer)(PVOID, TChar), PVOID context, const Error &error);
	/// @}

public:
	/**
	 * @brief Format string with variadic arguments
	 * @tparam TChar Character type (CHAR or WCHAR)
	 * @tparam Args Variadic argument types
	 * @param writer Callback function to write each character
	 * @param context User context passed to writer callback
	 * @param format Printf-style format string
	 * @param args Arguments to format
	 * @return Number of characters written
	 *
	 * @details Type-safe variadic template implementation. Arguments are
	 * automatically converted to the appropriate internal type.
	 */
	template <TCHAR TChar, typename... Args>
	static INT32 Format(BOOL (*writer)(PVOID, TChar), PVOID context, const TChar *format, Args &&...args);
};

template <TCHAR TChar>
INT32 StringFormatter::FormatInt64(BOOL (*writer)(PVOID, TChar), PVOID context, INT64 num, INT32 width, INT32 zeroPad, INT32 leftAlign)
{
	if (num < 0)
		return FormatUInt64<TChar>(writer, context, UINT64(0) - UINT64(num), width, zeroPad, leftAlign, (TChar)'-');
	return FormatUInt64<TChar>(writer, context, (UINT64)num, width, zeroPad, leftAlign, (TChar)0);
}

template <TCHAR TChar>
INT32 StringFormatter::FormatUInt64(BOOL (*writer)(PVOID, TChar), PVOID context, UINT64 num, INT32 width, INT32 zeroPad, INT32 leftAlign, TChar signChar)
{
	TChar rev[20];
	INT32 len = 0;
	INT32 index = 0;
	INT32 startIndex = index;

	// Convert number to reversed digit string
	do
	{
		rev[len++] = (TChar)((num % 10) + (UINT64)(UINT32)'0');
		num /= 10;
	} while (num);
 
	INT32 signWidth = signChar ? 1 : 0;
	INT32 paddingSpaces = width - len - signWidth;
	INT32 paddingZeros = 0;

	if (zeroPad && !leftAlign)
	{
		paddingZeros = paddingSpaces > 0 ? paddingSpaces : 0;
		paddingSpaces = 0;
	}
	else
	{
		paddingSpaces = paddingSpaces > 0 ? paddingSpaces : 0;
	}

	// Right-align: pad spaces first
	if (!leftAlign)
	{
		for (INT32 i = 0; i < paddingSpaces; ++i)
		{
			if (!writer(context, (TChar)' '))
				return index - startIndex;
			index++;
		}
	}

	// Output sign if present
	if (signChar)
	{
		if (!writer(context, signChar))
			return index - startIndex;
		index++;
	}

	// Leading zeros
	for (INT32 i = 0; i < paddingZeros; ++i)
	{
		if (!writer(context, (TChar)'0'))
			return index - startIndex;
		index++;
	}

	// Digits in correct order
	while (len)
	{
		if (!writer(context, rev[--len]))
			return index - startIndex;
		index++;
	}

	// Left-align: pad trailing spaces
	if (leftAlign)
	{
		INT32 printed = index - startIndex;
		for (INT32 i = printed; i < width; ++i)
		{
			if (!writer(context, (TChar)' '))
				return index - startIndex;
			index++;
		}
	}
	return index - startIndex;
}

template <TCHAR TChar>
INT32 StringFormatter::FormatUInt64AsHex(BOOL (*writer)(PVOID, TChar), PVOID context, UINT64 num, INT32 fieldWidth, INT32 uppercase, INT32 zeroPad, BOOL addPrefix)
{
	TChar buffer[16]; // Max 16 hex digits for UINT64
	INT32 buffIndex = 0;
	INT32 index = 0;
	INT32 startIdx = index;

	// Convert number to hex (reversed)
	if (num == 0)
	{
		buffer[buffIndex++] = (TChar)'0';
	}
	else
	{
		while (num)
		{
			UINT32 digit = (UINT32)(num & 0xF);
			TChar c;

			if (digit < 10)
				c = (TChar)('0' + digit);
			else
				c = (TChar)((uppercase ? 'A' : 'a') + (digit - 10));

			buffer[buffIndex++] = c;
			num >>= 4;
		}
	}

	INT32 prefixLen = addPrefix ? 2 : 0;
	INT32 totalDigits = buffIndex + prefixLen;
	INT32 pad = fieldWidth - totalDigits;
	if (pad < 0)
		pad = 0;

	// Space padding (right aligned) must come BEFORE prefix
	if (!zeroPad)
	{
		while (pad > 0)
		{
			if (!writer(context, (TChar)' '))
				return index - startIdx;
			index++;
			pad--;
		}
	}

	// Prefix
	if (addPrefix)
	{
		if (!writer(context, (TChar)'0'))
			return index - startIdx;
		index++;
		if (!writer(context, uppercase ? (TChar)'X' : (TChar)'x'))
			return index - startIdx;
		index++;
	}

	// Zero padding (after prefix, before digits)
	if (zeroPad)
	{
		while (pad > 0)
		{
			if (!writer(context, (TChar)'0'))
				return index - startIdx;
			index++;
			pad--;
		}
	}

	// Copy digits (reverse order)
	while (buffIndex)
	{
		if (!writer(context, buffer[--buffIndex]))
			return index - startIdx;
		index++;
	}

	return index - startIdx;
}

template <TCHAR TChar>
INT32 StringFormatter::FormatPointerAsHex(BOOL (*writer)(PVOID, TChar), PVOID context, PVOID ptr)
{
	return FormatUInt64AsHex<TChar>(writer, context, (UINT64)(USIZE)ptr, (INT32)(sizeof(USIZE) * 2), 0, 1, true);
}

template <TCHAR TChar>
INT32 StringFormatter::FormatWideString(BOOL (*writer)(PVOID, TChar), PVOID context, const WCHAR *wstr, INT32 fieldWidth, INT32 leftAlign)
{
	INT32 j = 0;
	if (wstr == nullptr)
	{
		if (!writer(context, (TChar)'?'))
			return 0;
		return 1;
	}

	// Measure string length for padding
	INT32 len = 0;
	for (INT32 k = 0; wstr[k] != (WCHAR)'\0'; k++)
		len++;

	INT32 padding = fieldWidth - len;
	if (padding < 0)
		padding = 0;

	// Right-align: leading spaces
	if (!leftAlign)
	{
		for (INT32 k = 0; k < padding; k++)
		{
			if (!writer(context, (TChar)' '))
				return j;
			j++;
		}
	}

	// Output string
	for (INT32 k = 0; wstr[k] != (WCHAR)'\0'; k++)
	{
		if (!writer(context, (TChar)wstr[k]))
			return j;
		j++;
	}

	// Left-align: trailing spaces
	if (leftAlign)
	{
		for (INT32 k = 0; k < padding; k++)
		{
			if (!writer(context, (TChar)' '))
				return j;
			j++;
		}
	}

	return j;
}

template <TCHAR TChar>
INT32 StringFormatter::FormatErrorEntry(BOOL (*writer)(PVOID, TChar), PVOID context,
	UINT32 code, Error::PlatformKind platform)
{
	INT32 j = 0;

	// Windows/UEFI: hex with 0x prefix (uppercase digits). Runtime/Posix: decimal.
	if (platform == Error::PlatformKind::Windows || platform == Error::PlatformKind::Uefi)
	{
		if (!writer(context, (TChar)'0'))
			return j;
		j++;
		if (!writer(context, (TChar)'x'))
			return j;
		j++;
		j += FormatUInt64AsHex<TChar>(writer, context, (UINT64)code, 0, 1, 0, false);
	}
	else
	{
		j += FormatUInt64<TChar>(writer, context, (UINT64)code, 0, 0, 0);
	}

	// Platform tag for non-Runtime: [W], [P], or [U]
	TChar tag = 0;
	if (platform == Error::PlatformKind::Windows)
		tag = (TChar)'W';
	else if (platform == Error::PlatformKind::Posix)
		tag = (TChar)'P';
	else if (platform == Error::PlatformKind::Uefi)
		tag = (TChar)'U';

	if (tag)
	{
		if (!writer(context, (TChar)'['))
			return j;
		j++;
		if (!writer(context, tag))
			return j;
		j++;
		if (!writer(context, (TChar)']'))
			return j;
		j++;
	}

	return j;
}

template <TCHAR TChar>
INT32 StringFormatter::FormatError(BOOL (*writer)(PVOID, TChar), PVOID context, const Error &error)
{
	INT32 j = 0;

	// Format the outermost (top-level) error
	j += FormatErrorEntry<TChar>(writer, context, (UINT32)error.Code, error.Platform);

	// Format inner cause chain separated by " <- "
	for (UINT8 i = 0; i < error.Depth; i++)
	{
		// Write " <- " separator
		if (!writer(context, (TChar)' '))
			return j;
		j++;
		if (!writer(context, (TChar)'<'))
			return j;
		j++;
		if (!writer(context, (TChar)'-'))
			return j;
		j++;
		if (!writer(context, (TChar)' '))
			return j;
		j++;

		j += FormatErrorEntry<TChar>(writer, context,
			error.InnerCodes[i], error.InnerPlatforms[i]);
	}

	return j;
}

template <TCHAR TChar>
INT32 StringFormatter::FormatDouble(
	BOOL (*writer)(PVOID, TChar),
	PVOID context,
	double num,
	INT32 precision,
	INT32 width,
	INT32 zeroPad)
{
	// Clamp precision to something safe for a small stack buffer
	if (precision < 0)
		precision = 0;
	if (precision > 32)
		precision = 32;

	// Handle NaN (portable check)
	if (num != num)
	{
		INT32 written = 0;
		INT32 pad = (width > 3) ? (width - 3) : 0;

		// Right-align: pad before "nan"
		for (INT32 i = 0; i < pad; ++i)
		{
			if (!writer(context, (TChar)' '))
				return written;
			written++;
		}
		if (!writer(context, (TChar)'n'))
			return written;
		written++;
		if (!writer(context, (TChar)'a'))
			return written;
		written++;
		if (!writer(context, (TChar)'n'))
			return written;
		written++;
		return written;
	}

	// Sign check
	BOOL isNegative = false;
	if (num < 0.0)
	{
		isNegative = true;
		num = -num;
	}

	// Rounding: num += 0.5 / 10^precision
	if (precision > 0)
	{
		double scale = 1.0;
		for (INT32 i = 0; i < precision; ++i)
			scale *= 10.0;
		num += (0.5 / scale);
	}
	else
	{
		// precision == 0 => round to integer
		num += 0.5;
	}

	// Build into a small local buffer, then emit through writer()
	// Max: sign(1) + 20 digits + '.' + 32 frac + a bit extra
	TChar tmp[80];
	INT32 len = 0;

	if (isNegative)
		tmp[len++] = (TChar)'-';

	// Integer part
	UINT64 intPart = (UINT64)num;
	double fracPart = num - (double)intPart;

	// Convert integer to reversed digits
	TChar intRev[32];
	INT32 intN = 0;

	if (intPart == 0)
	{
		intRev[intN++] = (TChar)'0';
	}
	else
	{
		while (intPart != 0 && intN < (INT32)(sizeof(intRev) / sizeof(intRev[0])))
		{
			UINT32 digit = (UINT32)(intPart % 10);
			intRev[intN++] = (TChar)((TChar)'0' + (TChar)digit);
			intPart /= 10;
		}
	}

	// Reverse into tmp
	for (INT32 i = intN - 1; i >= 0; --i)
		tmp[len++] = intRev[i];

	// Fractional part
	if (precision > 0)
	{
		tmp[len++] = (TChar)'.';
		for (INT32 i = 0; i < precision; ++i)
		{
			fracPart *= 10.0;
			INT32 d = (INT32)fracPart;
			if (d < 0)
				d = 0;
			if (d > 9)
				d = 9;
			tmp[len++] = (TChar)((TChar)'0' + (TChar)d);
			fracPart -= (double)d;
		}
	}

	// Right-align: pad BEFORE the number
	INT32 written = 0;
	if (width > len)
	{
		INT32 pad = width - len;
		if (zeroPad)
		{
			// Zero-pad: emit sign first, then zeros, then digits
			INT32 signLen = isNegative ? 1 : 0;
			if (signLen)
			{
				if (!writer(context, tmp[0]))
					return written;
				written++;
			}
			for (INT32 i = 0; i < pad; ++i)
			{
				if (!writer(context, (TChar)'0'))
					return written;
				written++;
			}
			for (INT32 i = signLen; i < len; ++i)
			{
				if (!writer(context, tmp[i]))
					return written;
				written++;
			}
		}
		else
		{
			// Space-pad before the number
			for (INT32 i = 0; i < pad; ++i)
			{
				if (!writer(context, (TChar)' '))
					return written;
				written++;
			}
			for (INT32 i = 0; i < len; ++i)
			{
				if (!writer(context, tmp[i]))
					return written;
				written++;
			}
		}
	}
	else
	{
		for (INT32 i = 0; i < len; ++i)
		{
			if (!writer(context, tmp[i]))
				return written;
			written++;
		}
	}

	return written;
}

// Variadic template implementation
template <TCHAR TChar, typename... Args>
INT32 StringFormatter::Format(BOOL (*writer)(PVOID, TChar), PVOID context, const TChar *format, Args &&...args)
{
	if constexpr (sizeof...(Args) == 0)
	{
		// No arguments, just copy the format string
		return FormatWithArgs<TChar>(writer, context, format, Span<const Argument>());
	}
	else
	{
		// Pack arguments into array
		Argument argArray[] = {Argument(args)...};
		return FormatWithArgs<TChar>(writer, context, format, Span<const Argument>(argArray));
	}
}

template <TCHAR TChar>
INT32 StringFormatter::FormatWithArgs(BOOL (*writer)(PVOID, TChar), PVOID context, const TChar *format, Span<const Argument> args)
{
	INT32 i = 0, j = 0;   // Index for the format string and output string
	INT32 precision = 6;  // Default precision for floating-point numbers
	INT32 currentArg = 0; // Current argument index

	// Validate the output string
	if (format == nullptr)
	{
		return 0;
	}

	// Loop through the format string to process each character
	while (format[i] != (TChar)'\0')
	{
		if (format[i] == (TChar)'%')
		{
			i++; // Skip '%'

			// Guard against trailing '%' at end of format string
			if (format[i] == (TChar)'\0')
				break;

			precision = 6; // Reset default precision

			// Parse flags: '-', '0', '#'
			INT32 addPrefix = 0;
			INT32 leftAlign = 0;
			INT32 zeroPad = 0;
			INT32 fieldWidth = 0;

			BOOL parsingFlags = true;
			while (parsingFlags)
			{
				if (format[i] == (TChar)'-')
				{
					leftAlign = 1;
					zeroPad = 0; // '-' overrides '0'
					i++;
				}
				else if (format[i] == (TChar)'0' && !leftAlign)
				{
					zeroPad = 1;
					i++;
				}
				else if (format[i] == (TChar)'#')
				{
					addPrefix = 1;
					i++;
				}
				else
				{
					parsingFlags = false;
				}
			}

			// Parse field width
			while (format[i] >= (TChar)'0' && format[i] <= (TChar)'9')
			{
				fieldWidth = fieldWidth * 10 + (format[i] - (TChar)'0');
				i++;
			}

			// Parse precision (e.g. "%.3f")
			if (format[i] == (TChar)'.')
			{
				i++; // Skip '.'
				precision = 0;
				while (format[i] >= (TChar)'0' && format[i] <= (TChar)'9')
				{
					precision = precision * 10 + (format[i] - (TChar)'0');
					i++;
				}
			}

			// Now switch based on the conversion specifier
			if (format[i] == (TChar)'X')
			{
				i++; // Skip 'X'
				if (currentArg >= (INT32)args.Size())
					continue;
				UINT32 num = args[currentArg++].U32;
				// Format the number as uppercase hexadecimal.
				j += StringFormatter::FormatUInt64AsHex(writer, context, (UINT64)num, fieldWidth, 1, zeroPad, addPrefix);

				// If a '-' follows, add it (for MAC address separators)
				if (format[i] == (TChar)'-')
				{
					if (!writer(context, (TChar)'-'))
						return j;
					j++;
					i++; // Skip the hyphen
				}
				continue;
			}
			// NOTE: making specifiers lowercase to handle both cases (e.g., %d and %D), that's why we use ToLowerCase function
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'f')
			{
				if (currentArg >= (INT32)args.Size())
				{
					i++;
					continue;
				}
				double num = args[currentArg++].Dbl;
				j += StringFormatter::FormatDouble(writer, context, num, precision, fieldWidth, zeroPad);
				i++; // Skip 'f'
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'d')
			{ // Handle %d (signed integer)
				if (currentArg >= (INT32)args.Size())
				{
					i++;
					continue;
				}
				INT32 num = args[currentArg++].I32;
				j += StringFormatter::FormatInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
				i++; // Skip 'd'
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'u')
			{ // Handle %u (unsigned integer)
				if (currentArg >= (INT32)args.Size())
				{
					i++;
					continue;
				}
				UINT32 num = args[currentArg++].U32;
				j += StringFormatter::FormatUInt64(writer, context, UINT64(num), fieldWidth, zeroPad, leftAlign);
				i++; // Skip 'u'
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'x')
			{ // Handle %x (hexadecimal, lowercase)
				if (currentArg >= (INT32)args.Size())
				{
					i++;
					continue;
				}
				UINT32 num = args[currentArg++].U32;
				j += StringFormatter::FormatUInt64AsHex(writer, context, (UINT64)num, fieldWidth, 0, zeroPad, addPrefix);
				i++; // Skip 'x'
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'p')
			{        // Handle %p (pointer)
				i++; // Skip 'p'
				if (currentArg >= (INT32)args.Size())
					continue;
				j += StringFormatter::FormatPointerAsHex(writer, context, args[currentArg++].Ptr);
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'c')
			{ // Handle %c (character)
				if (currentArg >= (INT32)args.Size())
				{
					i++;
					continue;
				}
				TChar ch = (TChar)args[currentArg++].I32;
				INT32 padding = fieldWidth > 1 ? fieldWidth - 1 : 0;

				if (!leftAlign)
				{
					for (INT32 k = 0; k < padding; k++)
					{
						if (!writer(context, (TChar)' '))
							return j;
						j++;
					}
				}
				if (!writer(context, ch))
					return j;
				j++;
				if (leftAlign)
				{
					for (INT32 k = 0; k < padding; k++)
					{
						if (!writer(context, (TChar)' '))
							return j;
						j++;
					}
				}
				i++; // Skip 'c'
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'s')
			{        // Handle %s (narrow string)
				i++; // Skip 's'
				if (currentArg >= (INT32)args.Size())
					continue;
				const CHAR *str = args[currentArg++].Cstr;
				if (str == nullptr)
				{
					if (!writer(context, (TChar)'?'))
						return j;
					j++;
					continue;
				}
				INT32 len = 0;
				const CHAR *temp = str;
				while (*temp)
				{
					len++;
					temp++;
				}
				INT32 padding = fieldWidth - len;
				if (padding < 0)
					padding = 0;

				if (!leftAlign)
				{
					for (INT32 k = 0; k < padding; k++)
					{
						if (!writer(context, (TChar)' '))
							return j;
						j++;
					}
				}
				while (*str)
				{
					if (!writer(context, (TChar)*str++))
						return j;
					j++;
				}
				if (leftAlign)
				{
					for (INT32 k = 0; k < padding; k++)
					{
						if (!writer(context, (TChar)' '))
							return j;
						j++;
					}
				}
				continue;
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'w')
			{ // Handle %ws (wide string)
				if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'s')
				{
					i += 2; // Skip over "ws"
					if (currentArg >= (INT32)args.Size())
						continue;
					j += FormatWideString<TChar>(writer, context, args[currentArg++].Wstr, fieldWidth, leftAlign);
					continue;
				}
				else
				{
					if (!writer(context, (TChar)format[i++]))
						return j;
					j++;
					continue;
				}
			}
			// Support %ls (wide string) in the same way as %ws
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'l')
			{
				if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'s')
				{
					i += 2; // Skip over "ls"
					if (currentArg >= (INT32)args.Size())
						continue;
					j += FormatWideString<TChar>(writer, context, args[currentArg++].Wstr, fieldWidth, leftAlign);
					continue;
				}
				// Handle other long variants (ld, lu, lld)
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'d')
				{           // long int (%ld)
					i += 2; // Skip over "ld"
					if (currentArg >= (INT32)args.Size())
						continue;
					// Type-aware read: on LLP64 (Windows) long is 32-bit, stored as INT32
					const Argument &arg = args[currentArg++];
					INT64 num = (arg.Kind == Argument::Type::INT64) ? arg.I64 : (INT64)arg.I32;
					j += StringFormatter::FormatInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'u')
				{           // unsigned long int (%lu)
					i += 2; // Skip over "lu"
					if (currentArg >= (INT32)args.Size())
						continue;
					// Type-aware read: on LLP64 (Windows) unsigned long is 32-bit, stored as UINT32
					const Argument &arg = args[currentArg++];
					UINT64 num = (arg.Kind == Argument::Type::UINT64) ? arg.U64 : (UINT64)arg.U32;
					j += StringFormatter::FormatUInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'l' && StringUtils::ToLowerCase<TChar>(format[i + 2]) == (TChar)'d')
				{           // long long int (%lld)
					i += 3; // Skip over "lld"
					if (currentArg >= (INT32)args.Size())
						continue;
					INT64 num = args[currentArg++].I64;
					j += StringFormatter::FormatInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'l' && StringUtils::ToLowerCase<TChar>(format[i + 2]) == (TChar)'u')
				{
					i += 3; // Skip over "llu"
					if (currentArg >= (INT32)args.Size())
						continue;
					UINT64 num = args[currentArg++].U64;
					j += StringFormatter::FormatUInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else if (format[i + 1] == (TChar)'X')
				{           // long uppercase hex (%lX)
					i += 2; // Skip over "lX"
					if (currentArg >= (INT32)args.Size())
						continue;
					const Argument &arg = args[currentArg++];
					UINT64 num = (arg.Kind == Argument::Type::UINT64) ? arg.U64 : (UINT64)arg.U32;
					j += StringFormatter::FormatUInt64AsHex(writer, context, num, fieldWidth, 1, zeroPad, addPrefix);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'x')
				{           // long hex (%lx)
					i += 2; // Skip over "lx"
					if (currentArg >= (INT32)args.Size())
						continue;
					const Argument &arg = args[currentArg++];
					UINT64 num = (arg.Kind == Argument::Type::UINT64) ? arg.U64 : (UINT64)arg.U32;
					j += StringFormatter::FormatUInt64AsHex(writer, context, num, fieldWidth, 0, zeroPad, addPrefix);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'l' && format[i + 2] == (TChar)'X')
				{           // long long uppercase hex (%llX)
					i += 3; // Skip over "llX"
					if (currentArg >= (INT32)args.Size())
						continue;
					UINT64 num = args[currentArg++].U64;
					j += StringFormatter::FormatUInt64AsHex(writer, context, num, fieldWidth, 1, zeroPad, addPrefix);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'l' && StringUtils::ToLowerCase<TChar>(format[i + 2]) == (TChar)'x')
				{           // long long hex (%llx)
					i += 3; // Skip over "llx"
					if (currentArg >= (INT32)args.Size())
						continue;
					UINT64 num = args[currentArg++].U64;
					j += StringFormatter::FormatUInt64AsHex(writer, context, num, fieldWidth, 0, zeroPad, addPrefix);
					continue;
				}
				else
				{
					if (!writer(context, format[i++]))
						return j;
					j++;
					continue;
				}
			}
			// Handle size_t variants (%zu, %zd) - USIZE/SSIZE are stored as UINT64/INT64
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'z')
			{
				if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'u')
				{           // unsigned size_t (%zu)
					i += 2; // Skip over "zu"
					if (currentArg >= (INT32)args.Size())
						continue;
					// Type-aware read: on 32-bit platforms USIZE is stored as UINT32
					const Argument &arg = args[currentArg++];
					UINT64 num = (arg.Kind == Argument::Type::UINT64) ? arg.U64 : (UINT64)arg.U32;
					j += StringFormatter::FormatUInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else if (StringUtils::ToLowerCase<TChar>(format[i + 1]) == (TChar)'d')
				{           // signed size_t (%zd)
					i += 2; // Skip over "zd"
					if (currentArg >= (INT32)args.Size())
						continue;
					// Type-aware read: on 32-bit platforms SSIZE is stored as INT32
					const Argument &arg = args[currentArg++];
					INT64 num = (arg.Kind == Argument::Type::INT64) ? arg.I64 : (INT64)arg.I32;
					j += StringFormatter::FormatInt64(writer, context, num, fieldWidth, zeroPad, leftAlign);
					continue;
				}
				else
				{
					if (!writer(context, format[i++]))
						return j;
					j++;
					continue;
				}
			}
			else if (StringUtils::ToLowerCase<TChar>(format[i]) == (TChar)'e')
			{        // Handle %e (error value)
				i++; // Skip 'e'
				if (currentArg >= (INT32)args.Size())
					continue;
				j += FormatError<TChar>(writer, context, args[currentArg++].ErrValue);
				continue;
			}
			else if (format[i] == (TChar)'%')
			{ // Handle literal "%%"
				if (!writer(context, (TChar)'%'))
					return j;
				j++;
				i++; // Skip the '%'
				continue;
			}
			else
			{ // Unknown specifier: output it as-is.
				if (!writer(context, format[i++]))
					return j;
				j++;
				continue;
			}
		}
		else
		{ // Ordinary character: copy it.
			if (!writer(context, format[i++]))
				return j;
			j++;
		}
	}
	return j; // Return the length of the formatted string
}

/** @} */ // end of formatter group
