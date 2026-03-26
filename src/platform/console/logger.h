/**
 * @file logger.h
 * @brief Structured logging with ANSI color support
 *
 * @details Provides log level filtering and colored console output without CRT
 * dependencies. All logging is performed via direct console syscalls with ANSI
 * escape sequences. Log levels include Info (green), Error (red), Warning
 * (yellow), and Debug (yellow). Type-erased arguments eliminate per-argument-type
 * template instantiations, and logging is zero-overhead when disabled at compile
 * time via the ENABLE_LOGGING and ENABLE_DEBUG_LOGGING preprocessor flags.
 */

#pragma once

#include "platform/platform.h"
#if defined(ENABLE_LOGGING)
// Convenience macros for logging
#define LOG_INFO(format, ...) Logger::Info<CHAR>(format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Logger::Error<CHAR>(format, ##__VA_ARGS__)
#define LOG_WARNING(format, ...) Logger::Warning<CHAR>(format, ##__VA_ARGS__)
#if defined(ENABLE_DEBUG_LOGGING)
#define LOG_DEBUG(format, ...) Logger::Debug<CHAR>(format, ##__VA_ARGS__)
#else
#define LOG_DEBUG(format, ...)
#endif // ENABLE_DEBUG_LOGGING
#else
// Define empty macros when logging is disabled
#define LOG_INFO(format, ...)
#define LOG_ERROR(format, ...)
#define LOG_DEBUG(format, ...)
#define LOG_WARNING(format, ...)
#endif // ENABLE_LOGGING

/**
 * Logger - Static logging utility class
 *
 * Public methods are variadic templates that type-erase arguments into
 * StringFormatter::Argument arrays, then format the message in the caller's
 * inline scope.
 *
 * DESIGN NOTE (Windows aarch64 -Oz):
 *   Three separate code-gen hazards require care here:
 *
 *   1. Stack-slot colouring: LLVM can merge PIC-transformed string allocas
 *      whose lifetimes appear non-overlapping.  Combined with instruction
 *      reordering this makes the format-string pointer read stale
 *      colour-prefix data.  Fix: no PIC string literals in prefix/reset;
 *      they are assembled from register immediates inside the NOINLINE
 *      helper.
 *
 *   2. Tail-call optimisation: a NOINLINE function whose last statement is
 *      Console::Write(Span) may be tail-called.  The epilogue tears down the
 *      frame *before* Console::Write dereferences the Span pointer, leaving
 *      the PIC data on a freed stack slot.  Fix: the reset sequence is
 *      written char-by-char in the inline caller, not in a NOINLINE helper.
 *
 *   3. PIC pointer escape: passing a PIC-stack pointer (format string) to a
 *      NOINLINE function can fail under -Oz because the compiler may
 *      optimise away the stack stores or reuse the slot.  Fix: FormatWithArgs
 *      is called in the inline template scope, never across a NOINLINE
 *      boundary.
 */
class Logger
{
private:
	/**
	 * ConsoleCallback - Callback for console output (with ANSI colors)
	 */
	static BOOL ConsoleCallbackA([[maybe_unused]] PVOID context, CHAR ch)
	{
		return Console::Write(Span<const CHAR>(&ch, 1));
	}

	/**
	 * WritePrefixAndTimestamp - NOINLINE helper that emits the coloured
	 * level tag and [HH:MM:SS] timestamp.
	 *
	 * The colour prefix is assembled from individual char stores (register
	 * immediates, not a PIC string literal) so there is no stack-slot or
	 * tail-call hazard.  The timestamp PIC string "[%s] " is the only
	 * string literal in this frame and is consumed before the function
	 * returns.
	 */
	static NOINLINE VOID WritePrefixAndTimestamp(CHAR colorDigit, CHAR l1, CHAR l2, CHAR l3)
	{
		// ── colour prefix  "\033[0;3Xm[LVL] " ──────────────────────────
		CHAR prefix[13];
		prefix[0]  = '\033';
		prefix[1]  = '[';
		prefix[2]  = '0';
		prefix[3]  = ';';
		prefix[4]  = '3';
		prefix[5]  = colorDigit;
		prefix[6]  = 'm';
		prefix[7]  = '[';
		prefix[8]  = l1;
		prefix[9]  = l2;
		prefix[10] = l3;
		prefix[11] = ']';
		prefix[12] = ' ';
		Console::Write(Span<const CHAR>(prefix, 13));

		// ── timestamp [HH:MM:SS] ────────────────────────────────────────
		DateTime now = DateTime::Now();
		TimeOnlyString<CHAR> timeStr = now.ToTimeOnlyString<CHAR>();
		StringFormatter::Format<CHAR>(&ConsoleCallbackA, nullptr, "[%s] ", (const CHAR *)timeStr);
	}

	/**
	 * WriteReset - Emit ANSI reset "\033[0m\n" char-by-char.
	 *
	 * MUST be called in the inline caller scope, not across a NOINLINE
	 * boundary.  A NOINLINE function whose last statement is Console::Write
	 * may be tail-called — the epilogue tears down the frame before the
	 * Span pointer is dereferenced.  Writing char-by-char avoids this.
	 */
	static VOID WriteReset()
	{
		ConsoleCallbackA(nullptr, '\033');
		ConsoleCallbackA(nullptr, '[');
		ConsoleCallbackA(nullptr, '0');
		ConsoleCallbackA(nullptr, 'm');
		ConsoleCallbackA(nullptr, '\n');
	}

	/**
	 * FormatBody - Format the message body from type-erased arguments.
	 *
	 * MUST remain inline (no NOINLINE) — the format string is a PIC-stack
	 * pointer that becomes invalid across a NOINLINE call boundary under
	 * -Oz.  The if-constexpr avoids zero-length array when no args.
	 */
	template <TCHAR TChar, typename... Args>
	static VOID FormatBody(const TChar *format, Args... args)
	{
		if constexpr (sizeof...(Args) == 0)
			StringFormatter::FormatWithArgs<CHAR>(&ConsoleCallbackA, nullptr, format, Span<const StringFormatter::Argument>());
		else
		{
			StringFormatter::Argument argArray[] = {StringFormatter::Argument(args)...};
			StringFormatter::FormatWithArgs<CHAR>(&ConsoleCallbackA, nullptr, format, Span<const StringFormatter::Argument>(argArray));
		}
	}

public:
	template <TCHAR TChar, typename... Args>
	static VOID Info(const TChar *format, Args... args)
	{
		WritePrefixAndTimestamp('2', 'I', 'N', 'F');
		FormatBody(format, args...);
		WriteReset();
	}

	template <TCHAR TChar, typename... Args>
	static VOID Error(const TChar *format, Args... args)
	{
		WritePrefixAndTimestamp('1', 'E', 'R', 'R');
		FormatBody(format, args...);
		WriteReset();
	}

	template <TCHAR TChar, typename... Args>
	static VOID Warning(const TChar *format, Args... args)
	{
		WritePrefixAndTimestamp('3', 'W', 'R', 'N');
		FormatBody(format, args...);
		WriteReset();
	}

	template <TCHAR TChar, typename... Args>
	static VOID Debug(const TChar *format, Args... args)
	{
		WritePrefixAndTimestamp('3', 'D', 'B', 'G');
		FormatBody(format, args...);
		WriteReset();
	}
};
