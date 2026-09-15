/**
 * @file console.h
 * @brief Console I/O abstraction
 *
 * @details Provides position-independent, printf-style console output without
 * depending on the C Runtime Library, standard I/O functions, dynamic memory
 * allocation, or the .rdata section. The Console class abstracts platform
 * differences, providing a unified interface for writing to stdout/stderr
 * using direct syscalls.
 *
 * On Windows, output is performed via NtDll syscalls (ZwWriteFile, with a
 * Kernel32 WriteFile fallback for pre-Windows 8 console pseudo-handles).
 * On Linux, output uses the write() syscall directly. All operations are
 * stack-based with no heap allocations, and type-safe via C++ templates for
 * compile-time character type dispatch (CHAR and WCHAR).
 */

#pragma once

#include "core/core.h"

/**
 * Console - Static class providing console I/O operations
 *
 * All methods are static - no instance needed. This is by design:
 *   1. No global state to initialize
 *   2. No vtable in .rdata
 *   3. Direct function calls (no virtual dispatch)
 *   4. Simplifies position-independent code
 */
class Console
{
private:
	/**
	 * FormatterCallback - Internal callback for character-by-character output
	 *
	 * Used by StringFormatter to emit formatted characters one at a time.
	 * This callback is invoked for each character in the formatted output.
	 *
	 * @param context - User context (unused, reserved for future use)
	 * @param ch      - Character to write to console
	 * @return true on success, false on error
	 *
	 * TEMPLATE PARAMETER:
	 *   TChar - Character type (CHAR or WCHAR) determined at compile-time
	 *
	 * POSITION-INDEPENDENT NOTE:
	 *   Function pointer references are automatically transformed to
	 *   PC-relative asm by the pic-transform LLVM pass.
	 */
	template <TCHAR TChar>
	static BOOL FormatterCallback(PVOID context, TChar ch);

public:
	/**
	 * Write - Output narrow (ANSI) string to console
	 *
	 * Platform Behavior:
	 *   Windows: Writes to the standard output handle (ZwWriteFile; kernel32
	 *            WriteFile fallback when the handle is a pre-Win8 console
	 *            pseudo-handle that ZwWriteFile rejects)
	 *   Linux:   Writes directly via write(STDOUT_FILENO, text, length)
	 *
	 * @param text   - Pointer to narrow character string
	 * @param length - Number of characters to write
	 * @return Number of characters written, 0 on error
	 *
	 * SYSCALL IMPLEMENTATION:
	 *   Windows: NtDll!ZwWriteFile or Kernel32!WriteFile (legacy consoles)
	 *   Linux:   syscall(__NR_write, 1, text, length)
	 */
	static NOINLINE UINT32 Write(Span<const CHAR> text);

	/**
	 * Write - Output wide (Unicode) string to console
	 *
	 * Platform Behavior:
	 *   Windows: Converts UTF-16 → UTF-8, then the narrow Write path
	 *   Linux:   Converts UTF-16 → UTF-8, then write() syscall
	 *
	 * @param text - Span of wide characters to write
	 * @return Number of characters written, 0 on error
	 *
	 * UNICODE HANDLING:
	 *   - Windows: Full Unicode support including emoji, CJK, etc.
	 *   - Linux:   Converts to UTF-8 (handles surrogate pairs correctly)
	 *
	 * IMPORTANT:
	 *   Size is in characters, not bytes!
	 *   For WCHAR, each character is sizeof(WCHAR) bytes
	 */
	static NOINLINE UINT32 Write(Span<const WCHAR> text);

	/**
	 * Write - Output null-terminated string to console (template version)
	 *
	 * Convenience wrapper that automatically calculates string length.
	 * Ideal for use with embedded strings.
	 *
	 * @tparam TChar Character type (CHAR or WCHAR), automatically deduced from argument
	 * @param text - Null-terminated string (CHAR* or WCHAR*)
	 * @return Number of characters written
	 *
	 * USAGE:
	 *   Console::Write<WCHAR>(L"Hello");    // Wide string
	 *   Console::Write<CHAR>("Hello");      // Narrow string
	 *
	 * PERFORMANCE NOTE:
	 *   Calls StringUtils::Length() to find null terminator - O(n) operation.
	 *   If you already know the length, use the length-based overload instead.
	 */
	template <TCHAR TChar>
	static UINT32 Write(const TChar *text);

	/**
	 * WriteFormatted - Printf-style formatted output using C++11 variadic templates
	 *
	 * Supports standard printf format specifiers:
	 *   %d    - Signed decimal integer
	 *   %u    - Unsigned decimal integer
	 *   %ld   - Long signed decimal integer
	 *   %X    - Uppercase hexadecimal
	 *   %x    - Lowercase hexadecimal
	 *   %f    - Floating-point (default precision)
	 *   %.Nf  - Floating-point with N decimal places
	 *   %c    - Single character
	 *   %s    - Narrow string (CHAR*)
	 *   %ls   - Wide string (WCHAR*)
	 *   %p    - Pointer (hexadecimal)
	 *
	 * @tparam TChar Character type for format string (CHAR or WCHAR)
	 * @tparam Args Variadic template arguments (deduced automatically)
	 * @param format - Format string with embedded specifiers
	 * @param args   - Variable arguments matching format specifiers (supports custom types!)
	 * @return Number of characters written
	 *
	 * USAGE:
	 *   Console::WriteFormatted<WCHAR>(
	 *       L"Value: %d, Pi: %.5f\n",
	 *       42,
	 *       3.14159
	 *   );
	 *
	 * POSITION-INDEPENDENT IMPLEMENTATION:
	 *   - Format string embedded in .text (no .rdata)
	 *   - Floating-point constants as immediates
	 *   - Stack-based buffer for formatting
	 *   - No heap allocations
	 *   - Type-safe variadic templates (no VA_LIST)
	 */
	template <TCHAR TChar, typename... Args>
	static UINT32 WriteFormatted(const TChar *format, Args &&...args);
};

// ============================================================================
// TEMPLATE IMPLEMENTATIONS
// ============================================================================

/**
 * Write<TChar> - Null-terminated string output (inline implementation)
 *
 * This template is instantiated at compile-time for each character type.
 * The compiler generates two versions:
 *   1. Console::Write<CHAR>(const CHAR* text)
 *   2. Console::Write<WCHAR>(const WCHAR* text)
 */
template <TCHAR TChar>
UINT32 Console::Write(const TChar *text)
{
	// Calculate string length and forward to Span-based overload
	// StringUtils::Length() walks the string until it finds the null terminator
	return Write(Span<const TChar>(text, StringUtils::Length(text)));
}

/**
 * FormatterCallback<TChar> - Character emission callback (inline implementation)
 *
 * StringFormatter calls this function for each formatted character.
 * We simply write each character directly to the console.
 *
 * DESIGN RATIONALE:
 *   Why not buffer the output?
 *   - Buffering requires allocating memory (heap or large stack)
 *   - Character-by-character is simpler and more position-independent
 *   - Kernel handles buffering internally anyway
 *   - For small outputs, overhead is negligible
 */
template <TCHAR TChar>
BOOL Console::FormatterCallback([[maybe_unused]] PVOID context, TChar ch)
{
	// Write single character to console
	// Return value: true = success, false = error
	return Write(Span<const TChar>(&ch, 1));
}

/**
 * WriteFormatted<TChar, Args...> - Variadic template formatting implementation
 *
 * This function uses C++11 variadic templates for type-safe formatting.
 * No more VA_LIST or VA_ARG - everything is type-safe at compile time!
 *
 * Function pointer references (e.g. &FormatterCallback<TChar>) are automatically
 * transformed to PC-relative asm by the pic-transform LLVM pass.
 *
 * BENEFITS OVER VA_LIST:
 *   - Type-safe: compiler knows exact types at compile time
 *   - Supports custom types without casting
 *   - No POD requirement
 *   - Better optimization opportunities
 */
template <TCHAR TChar, typename... Args>
UINT32 Console::WriteFormatted(const TChar *format, Args &&...args)
{
	return StringFormatter::Format(&FormatterCallback<TChar>, nullptr, format, static_cast<Args &&>(args)...);
}
