/**
 * @file environment.cc
 * @brief Shared POSIX environment variable and platform implementation
 *
 * @details Linux reads environment variables from /proc/self/environ.
 * macOS, FreeBSD, and Solaris return 0 (not found) as they lack a simple
 * procfs-based mechanism in freestanding mode.
 *
 * Platform identification:
 * - GetAgentPlatform(): compile-time OS target from PLATFORM_* defines
 * - GetOSVersion(): runtime OS version via uname syscall (Linux/Android)
 *   or /proc/version fallback (other POSIX platforms)
 *
 * Future enhancements:
 * - macOS: use sysctl(kern.procargs2) to read process environment
 * - FreeBSD: use sysctl(kern.proc.env) to read process environment
 * - Solaris: use /proc/self/psinfo + /proc/self/as or walk the initial stack
 */

#include "platform/system/environment.h"
#include "core/string/string.h"

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)

#if defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#else
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#endif
#include "core/memory/memory.h"

// Helper to compare strings (case-sensitive for Linux)
static BOOL CompareEnvName(const CHAR *envEntry, const CHAR *name) noexcept
{
	while (*name != '\0')
	{
		if (*envEntry != *name)
		{
			return false;
		}
		envEntry++;
		name++;
	}

	// After name, should be '='
	return *envEntry == '=';
}

USIZE Environment::GetVariable(const CHAR *name, Span<CHAR> buffer) noexcept
{
	if (name == nullptr || buffer.Size() == 0)
	{
		return 0;
	}

	// Open /proc/self/environ
	const CHAR *procEnvPath = "/proc/self/environ";
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	// aarch64/riscv only has openat syscall
	SSIZE fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)procEnvPath, 0, 0);
	if (fd < 0)
	{
		buffer[0] = '\0';
		return 0;
	}
#else
	SSIZE fd = System::Call(SYS_OPEN, (USIZE)procEnvPath, 0 /* O_RDONLY */, 0);
	if (fd < 0)
	{
		// Try openat with AT_FDCWD (-100) for newer kernels
		fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)procEnvPath, 0, 0);
		if (fd < 0)
		{
			buffer[0] = '\0';
			return 0;
		}
	}
#endif

	// Read environment block (entries separated by null bytes)
	CHAR envBuf[4096];
	SSIZE bytesRead = System::Call(SYS_READ, (USIZE)fd, (USIZE)envBuf, sizeof(envBuf) - 1);
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (bytesRead <= 0)
	{
		buffer[0] = '\0';
		return 0;
	}

	envBuf[bytesRead] = '\0';

	// Search for the variable
	const CHAR *ptr = envBuf;
	const CHAR *end = envBuf + bytesRead;

	while (ptr < end && *ptr != '\0')
	{
		if (CompareEnvName(ptr, name))
		{
			// Find the '=' and skip past it
			const CHAR *value = ptr;
			while (*value != '=' && *value != '\0')
			{
				value++;
			}
			if (*value == '=')
			{
				value++; // Skip the '='

				// Copy value to buffer
				USIZE len = 0;
				while (*value != '\0' && len < buffer.Size() - 1)
				{
					buffer[len++] = *value++;
				}
				buffer[len] = '\0';
				return len;
			}
		}

		// Skip to next entry (after null terminator)
		while (ptr < end && *ptr != '\0')
		{
			ptr++;
		}
		ptr++; // Skip the null terminator
	}

	// Variable not found
	buffer[0] = '\0';
	return 0;
}

#else // macOS, FreeBSD, Solaris — stub implementation

USIZE Environment::GetVariable(const CHAR *name, Span<CHAR> buffer) noexcept
{
	if (name == nullptr || buffer.Size() == 0)
	{
		return 0;
	}

	// No /proc filesystem or equivalent available in freestanding mode.
	// Return empty result.
	buffer[0] = '\0';
	return 0;
}

#endif

// =============================================================================
// Platform identification (shared across all POSIX platforms)
// =============================================================================

USIZE Environment::GetAgentPlatform(Span<CHAR> buffer) noexcept
{
#if defined(PLATFORM_LINUX)
	StringUtils::Copy(buffer, Span<const CHAR>("linux"));
#elif defined(PLATFORM_MACOS)
	StringUtils::Copy(buffer, Span<const CHAR>("macos"));
#elif defined(PLATFORM_ANDROID)
	StringUtils::Copy(buffer, Span<const CHAR>("android"));
#elif defined(PLATFORM_IOS)
	StringUtils::Copy(buffer, Span<const CHAR>("ios"));
#elif defined(PLATFORM_FREEBSD)
	StringUtils::Copy(buffer, Span<const CHAR>("freebsd"));
#elif defined(PLATFORM_SOLARIS)
	StringUtils::Copy(buffer, Span<const CHAR>("solaris"));
#else
	StringUtils::Copy(buffer, Span<const CHAR>("unknown"));
#endif
	return StringUtils::Length(buffer.Data());
}

USIZE Environment::GetOSVersion(Span<CHAR> buffer) noexcept
{
	if (buffer.Size() == 0)
		return 0;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	// Use the uname syscall to get kernel release info
	Utsname uts;
	Memory::Zero(&uts, sizeof(Utsname));
	SSIZE ret = System::Call(SYS_UNAME, (USIZE)&uts);
	if (ret == 0)
	{
		// Format: "{sysname} {release}" e.g. "Linux 6.1.0"
		USIZE sysLen = StringUtils::Length(uts.Sysname);
		USIZE relLen = StringUtils::Length(uts.Release);
		USIZE pos = 0;

		StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(uts.Sysname, sysLen + 1));
		pos += sysLen;

		buffer.Data()[pos++] = ' ';

		StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(uts.Release, relLen + 1));
		pos += relLen;
		return pos;
	}
#endif

	// Fallback: try reading /proc/version via raw syscalls (Linux/Android only)
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	{
		const CHAR *path = "/proc/version";
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
		SSIZE fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)path, 0, 0);
#else
		SSIZE fd = System::Call(SYS_OPEN, (USIZE)path, 0, 0);
#endif
		if (fd >= 0)
		{
			SSIZE bytesRead = System::Call(SYS_READ, (USIZE)fd, (USIZE)buffer.Data(), buffer.Size() - 1);
			System::Call(SYS_CLOSE, (USIZE)fd);
			if (bytesRead > 0)
			{
				// Trim trailing newline
				if (buffer.Data()[bytesRead - 1] == '\n')
					buffer.Data()[bytesRead - 1] = '\0';
				else
					buffer.Data()[bytesRead] = '\0';
				return StringUtils::Length(buffer.Data());
			}
		}
	}
#endif

	StringUtils::Copy(buffer, Span<const CHAR>("unknown"));
	return StringUtils::Length(buffer.Data());
}
