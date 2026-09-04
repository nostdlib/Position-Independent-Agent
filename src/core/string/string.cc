#include "core/string/string.h"

// ============================================================================
// NUMBER CONVERSION IMPLEMENTATIONS
// ============================================================================

USIZE StringUtils::FloatToStr(double value, Span<CHAR> buffer, UINT8 precision) noexcept
{
	if (buffer.Size() < 2)
		return 0;
	if (precision > 15)
		precision = 15;

	USIZE pos = 0;

	// Handle negative
	if (value < 0.0)
	{
		if (pos < buffer.Size() - 1)
			buffer[pos++] = '-';
		value = -value;
	}

	// Rounding: add 0.5 / 10^precision
	if (precision > 0)
	{
		double scale = 1.0;
		for (UINT8 p = 0; p < precision; p++)
			scale = scale * 10.0;
		value = value + 5.0 / (scale * 10.0);
	}
	else
	{
		value = value + 0.5;
	}

	// Integer part
	UINT64 intPart = (UINT64)value;
	double fracPart = value - (double)intPart;

	CHAR intBuf[24];
	USIZE intLen = UIntToStr(intPart, Span<CHAR>(intBuf));
	for (USIZE i = 0; i < intLen && pos < buffer.Size() - 1; i++)
		buffer[pos++] = intBuf[i];

	// Fractional part
	if (precision > 0 && pos < buffer.Size() - 1)
	{
		buffer[pos++] = '.';

		for (UINT8 p = 0; p < precision && pos < buffer.Size() - 1; p++)
		{
			fracPart = fracPart * 10.0;
			INT32 digit = (INT32)fracPart;
			if (digit < 0)
				digit = 0;
			if (digit > 9)
				digit = 9;
			buffer[pos++] = '0' + digit;
			fracPart = fracPart - (double)digit;
		}

		// Trim trailing zeros
		while (pos > 2 && buffer[pos - 1] == '0' && buffer[pos - 2] != '.')
			pos--;
	}

	buffer[pos] = '\0';
	return pos;
}

Result<double, Error> StringUtils::StrToFloat(Span<const CHAR> str) noexcept
{
	if (str.Size() == 0)
	{
		return Result<double, Error>::Err(Error::String_ParseFloatFailed);
	}

	// Validate that the string contains at least one digit
	BOOL hasDigit = false;
	for (USIZE i = 0; i < str.Size(); i++)
	{
		if (str[i] >= '0' && str[i] <= '9')
		{
			hasDigit = true;
			break;
		}
	}

	if (!hasDigit)
	{
		return Result<double, Error>::Err(Error::String_ParseFloatFailed);
	}

	USIZE i = 0;
	double sign = 1.0;
	double result = 0.0;
	double frac = 0.0;
	double base = 1.0;

	// sign
	if (str[i] == '-')
	{
		sign = -1.0;
		i++;
	}
	else if (str[i] == '+')
	{
		i++;
	}

	// integer part
	while (i < str.Size() && str[i] >= '0' && str[i] <= '9')
	{
		result = result * 10.0 + (double)(str[i] - '0');
		i++;
	}

	// fractional part
	if (i < str.Size() && str[i] == '.')
	{
		i++;
		while (i < str.Size() && str[i] >= '0' && str[i] <= '9')
		{
			frac = frac * 10.0 + (double)(str[i] - '0');
			base = base * 10.0;
			i++;
		}
	}

	double parsed = sign * (result + frac / base);
	return Result<double, Error>::Ok(parsed);
}

// ============================================================================
// UTF CONVERSION IMPLEMENTATIONS
// ============================================================================

// Converts a UTF-8 string to wide string (UTF-16 on Windows, UCS-4 on Linux)
// Returns the number of wide characters written (excluding null terminator)
USIZE StringUtils::Utf8ToWide(Span<const CHAR> utf8, Span<WCHAR> wide)
{
	if (utf8.Size() == 0 || wide.Size() < 3)
	{
		if (wide.Size() > 0)
			wide[0] = L'\0';
		return 0;
	}

	USIZE wideLen = 0;
	USIZE i = 0;

	while (i < utf8.Size() && utf8[i] != '\0' && wideLen + 2 < wide.Size())
	{
		UINT32 ch;
		UINT8 byte = (UINT8)utf8[i++];

		if (byte < 0x80)
		{
			ch = byte;
		}
		else if ((byte & 0xE0) == 0xC0)
		{
			ch = (byte & 0x1F) << 6;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F);
		}
		else if ((byte & 0xF0) == 0xE0)
		{
			ch = (byte & 0x0F) << 12;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F) << 6;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F);
		}
		else if ((byte & 0xF8) == 0xF0)
		{
			ch = (byte & 0x07) << 18;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F) << 12;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F) << 6;
			if (i < utf8.Size() && utf8[i] != '\0')
				ch |= (utf8[i++] & 0x3F);

			if (ch >= 0x10000)
			{
				if constexpr (sizeof(WCHAR) >= 4)
				{
					// UCS-4: store full codepoint directly
					wide[wideLen++] = (WCHAR)ch;
				}
				else
				{
					// UTF-16: encode as surrogate pair
					ch -= 0x10000;
					wide[wideLen++] = (WCHAR)(0xD800 + (ch >> 10));
					wide[wideLen++] = (WCHAR)(0xDC00 + (ch & 0x3FF));
				}
				continue;
			}
		}
		else
		{
			continue; // Invalid UTF-8 byte, skip
		}

		wide[wideLen++] = (WCHAR)ch;
	}

	wide[wideLen] = L'\0';
	return wideLen;
}

// Maps one byte (always >= 0x80 — ASCII never needs escaping) to the lone
// low surrogate 0xDC00 + byte, i.e. U+DC80..U+DCFF (Python "surrogateescape";
// bijective over the escaped range, reversed exactly by UTF16::ToUTF8Lossless).
static VOID EscapeByte(UINT8 byte, Span<WCHAR> wide, USIZE &wideLen)
{
	wide[wideLen++] = (WCHAR)(0xDC00 + byte);
}

// Converts a UTF-8 string to wide string, mapping every byte that is not part
// of a well-formed UTF-8 sequence (RFC 3629) to 0xDC00 + byte (U+DC80..U+DCFF) instead of
// skipping it — a filename is data, not text, and must survive the round trip.
// Returns the number of wide characters written (excluding null terminator).
USIZE StringUtils::Utf8ToWideLossless(Span<const CHAR> utf8, Span<WCHAR> wide)
{
	if (utf8.Size() == 0 || wide.Size() < 3)
	{
		if (wide.Size() > 0)
			wide[0] = L'\0';
		return 0;
	}

	USIZE wideLen = 0;
	USIZE i = 0;

	// One slot per unit (plus the NUL); only a UTF-16 surrogate pair needs two,
	// checked in that branch — a blanket +2 guard truncated 255-char names.
	while (i < utf8.Size() && utf8[i] != '\0' && wideLen + 1 < wide.Size())
	{
		UINT8 byte = (UINT8)utf8[i];
		UINT32 ch;
		USIZE seqLen;

		if (byte < 0x80)
		{
			ch = byte;
			seqLen = 1;
		}
		else if (byte >= 0xC2 && byte <= 0xDF) // 2-byte (C0/C1 are always overlong)
		{
			ch = (byte & 0x1F) << 6;
			seqLen = 2;
		}
		else if (byte >= 0xE0 && byte <= 0xEF) // 3-byte
		{
			ch = (byte & 0x0F) << 12;
			seqLen = 3;
		}
		else if (byte >= 0xF0 && byte <= 0xF4) // 4-byte (F5+ encode > U+10FFFF)
		{
			ch = (byte & 0x07) << 18;
			seqLen = 4;
		}
		else
		{
			// Invalid lead byte (bare continuation 0x80–0xBF, overlong leads
			// 0xC0/0xC1, 0xF5–0xFF): escape it alone; the bytes after it get
			// re-examined as potential sequence starts on later iterations.
			EscapeByte(byte, wide, wideLen);
			i++;
			continue;
		}

		// Assemble the continuations; a missing or non-continuation byte makes
		// the whole sequence malformed.
		BOOL valid = true;
		for (USIZE j = 1; j < seqLen; j++)
		{
			if (i + j >= utf8.Size() || utf8[i + j] == '\0' || ((((UINT8)utf8[i + j]) & 0xC0) != 0x80))
			{
				valid = false;
				break;
			}
			ch |= ((UINT8)utf8[i + j] & 0x3F) << (6 * (seqLen - 1 - j));
		}

		// Range validation subsumes every per-lead bound of RFC 3629 Section 4:
		// overlong encodings land below the minimum, CESU-8 surrogates land in
		// the forbidden D800–DFFF window, and > U+10FFFF lands above the max.
		if (valid && seqLen == 2 && ch < 0x80)
			valid = false;
		if (valid && seqLen == 3 && (ch < 0x800 || (ch >= 0xD800 && ch <= 0xDFFF)))
			valid = false;
		if (valid && seqLen == 4 && (ch < 0x10000 || ch > 0x10FFFF))
			valid = false;

		if (!valid)
		{
			// Escape only the lead byte; any offending continuation bytes are
			// re-examined next iteration (a bare continuation is an invalid
			// lead and gets escaped there) — no input byte is ever dropped.
			EscapeByte(byte, wide, wideLen);
			i++;
			continue;
		}

		i += seqLen;

		if (seqLen == 4)
		{
			if constexpr (sizeof(WCHAR) >= 4)
			{
				wide[wideLen++] = (WCHAR)ch; // UCS-4: one unit
			}
			else
			{
				if (wideLen + 2 >= wide.Size())
					break; // no room for a UTF-16 surrogate pair
				ch -= 0x10000;
				wide[wideLen++] = (WCHAR)(0xD800 + (ch >> 10));
				wide[wideLen++] = (WCHAR)(0xDC00 + (ch & 0x3FF));
			}
			continue;
		}

		wide[wideLen++] = (WCHAR)ch;
	}

	wide[wideLen] = L'\0';
	return wideLen;
}
