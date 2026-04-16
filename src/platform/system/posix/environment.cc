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
 * - GetOSVersion(): runtime OS version via uname syscall (Linux/Android),
 *   sysctl (macOS/iOS/FreeBSD), utssys then /etc/release (Solaris), or 0 on failure
 * - GetHostname(): HOSTNAME env var (Linux/Android), /etc/hostname (Linux/Android),
 *   sysctl kern.hostname (macOS/iOS/FreeBSD), utssys then /etc/nodename (Solaris)
 *
 * Future enhancements:
 * - macOS: use sysctl(kern.procargs2) to read process environment
 * - FreeBSD: use sysctl(kern.proc.env) to read process environment
 * - Solaris: use /proc/self/psinfo + /proc/self/as or walk the initial stack
 */

#include "platform/system/environment.h"
#include "core/string/string.h"

// Platform-specific kernel headers
#if defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#elif defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#elif defined(PLATFORM_MACOS)
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#elif defined(PLATFORM_IOS)
#include "platform/kernel/ios/syscall.h"
#include "platform/kernel/ios/system.h"
#elif defined(PLATFORM_FREEBSD)
#include "platform/kernel/freebsd/syscall.h"
#include "platform/kernel/freebsd/system.h"
#elif defined(PLATFORM_SOLARIS)
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"
#endif

#include "core/memory/memory.h"

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)

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
// Command line argument parsing (shared across all POSIX platforms)
// =============================================================================

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)

USIZE Environment::GetCommandLineValue(const CHAR *flag, Span<CHAR> buffer) noexcept
{
	if (flag == nullptr || buffer.Size() == 0)
		return 0;

	// Read /proc/self/cmdline (NUL-separated arguments)
	const CHAR *path = "/proc/self/cmdline";
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	SSIZE fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)path, 0, 0);
#else
	SSIZE fd = System::Call(SYS_OPEN, (USIZE)path, 0 /* O_RDONLY */, 0);
	if (fd < 0)
		fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)path, 0, 0);
#endif
	if (fd < 0)
	{
		buffer[0] = '\0';
		return 0;
	}

	CHAR cmdBuf[4096];
	SSIZE bytesRead = System::Call(SYS_READ, (USIZE)fd, (USIZE)cmdBuf, sizeof(cmdBuf) - 1);
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (bytesRead <= 0)
	{
		buffer[0] = '\0';
		return 0;
	}

	// Walk NUL-separated arguments looking for the flag
	USIZE flagLen = StringUtils::Length(flag);
	const CHAR *ptr = cmdBuf;
	const CHAR *end = cmdBuf + bytesRead;

	while (ptr < end)
	{
		USIZE argLen = StringUtils::Length(ptr);

		if (StringUtils::Compare(ptr, flag))
		{
			// Found the flag — advance to the next argument (the value)
			ptr += argLen + 1; // skip past NUL
			if (ptr >= end || *ptr == '\0')
			{
				buffer[0] = '\0';
				return 0;
			}

			USIZE valueLen = StringUtils::Length(ptr);
			if (valueLen >= buffer.Size())
				valueLen = buffer.Size() - 1;
			for (USIZE i = 0; i < valueLen; i++)
				buffer[i] = ptr[i];
			buffer[valueLen] = '\0';
			return valueLen;
		}

		// Check if the flag is a prefix (e.g. "--relay=https://...")
		if (argLen > flagLen && StringUtils::StartsWith(ptr, flag) && ptr[flagLen] == '=')
		{
			const CHAR *value = ptr + flagLen + 1;
			USIZE valueLen = StringUtils::Length(value);
			if (valueLen >= buffer.Size())
				valueLen = buffer.Size() - 1;
			for (USIZE i = 0; i < valueLen; i++)
				buffer[i] = value[i];
			buffer[valueLen] = '\0';
			return valueLen;
		}

		// Advance to next argument
		ptr += argLen + 1;
	}

	buffer[0] = '\0';
	return 0;
}

#else // macOS, FreeBSD, Solaris — stub

USIZE Environment::GetCommandLineValue([[maybe_unused]] const CHAR *flag, Span<CHAR> buffer) noexcept
{
	if (buffer.Size() > 0)
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
	buffer.Data()[0] = '\0';
	return 0;
#endif
	return StringUtils::Length(buffer.Data());
}

Result<USIZE, Error> Environment::GetOSVersion(Span<CHAR> buffer) noexcept
{
	if (buffer.Size() == 0)
		return Result<USIZE, Error>::Err(Error(Error::None));

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
		return Result<USIZE, Error>::Ok(pos);
	}

	// Fallback: try reading /proc/version via raw syscalls
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
				return Result<USIZE, Error>::Ok(StringUtils::Length(buffer.Data()));
			}
		}
	}
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
	// Use sysctl to query kern.ostype and kern.osrelease
	// sysctl(name, namelen, oldp, oldlenp, newp, newlen)
	{
		INT32 mib[2];
		CHAR ostype[128];
		CHAR osrelease[128];
		USIZE len;

		// CTL_KERN=1, KERN_OSTYPE=1 → e.g. "Darwin" or "FreeBSD"
		mib[0] = 1;
		mib[1] = 1;
		len = sizeof(ostype) - 1;
		Memory::Zero(ostype, sizeof(ostype));
		SSIZE ret = System::Call(SYS_SYSCTL, (USIZE)mib, 2, (USIZE)ostype, (USIZE)&len, 0, 0);
		if (ret < 0)
			return Result<USIZE, Error>::Err(Error(Error::None));

		// CTL_KERN=1, KERN_OSRELEASE=2 → e.g. "23.1.0" or "14.0-RELEASE"
		mib[1] = 2;
		len = sizeof(osrelease) - 1;
		Memory::Zero(osrelease, sizeof(osrelease));
		ret = System::Call(SYS_SYSCTL, (USIZE)mib, 2, (USIZE)osrelease, (USIZE)&len, 0, 0);
		if (ret < 0)
			return Result<USIZE, Error>::Err(Error(Error::None));

		// Format: "{ostype} {osrelease}" e.g. "Darwin 23.1.0"
		USIZE sysLen = StringUtils::Length(ostype);
		USIZE relLen = StringUtils::Length(osrelease);
		USIZE pos = 0;

		StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(ostype, sysLen + 1));
		pos += sysLen;

		buffer.Data()[pos++] = ' ';

		StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(osrelease, relLen + 1));
		pos += relLen;
		return Result<USIZE, Error>::Ok(pos);
	}
#elif defined(PLATFORM_SOLARIS)
	// Try utssys syscall first (works on illumos/OpenIndiana)
	// utssys(buf, 0, UTS_UNAME=0)
	{
		SolarisUtsname uts;
		Memory::Zero(&uts, sizeof(SolarisUtsname));
		SSIZE ret = System::Call(SYS_UTSSYS, (USIZE)&uts, 0, 0);
		if (ret == 0 && uts.Sysname[0] != '\0')
		{
			// Format: "{sysname} {release}" e.g. "SunOS 5.11"
			USIZE sysLen = StringUtils::Length(uts.Sysname);
			USIZE relLen = StringUtils::Length(uts.Release);
			USIZE pos = 0;

			StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(uts.Sysname, sysLen + 1));
			pos += sysLen;

			buffer.Data()[pos++] = ' ';

			StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(uts.Release, relLen + 1));
			pos += relLen;
			return Result<USIZE, Error>::Ok(pos);
		}
	}

	// Fallback: read /etc/release (always present on Oracle Solaris 11.4
	// where utssys may be removed/repurposed)
	{
		const CHAR *path = "/etc/release";
		SSIZE fd = System::Call(SYS_OPEN, (USIZE)path, 0 /* O_RDONLY */, 0);
		if (fd < 0)
			fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, 0, 0);
		if (fd >= 0)
		{
			CHAR tmpBuf[256];
			SSIZE bytesRead = System::Call(SYS_READ, (USIZE)fd, (USIZE)tmpBuf, sizeof(tmpBuf) - 1);
			System::Call(SYS_CLOSE, (USIZE)fd);
			if (bytesRead > 0)
			{
				tmpBuf[bytesRead] = '\0';

				// Skip leading whitespace on first line
				const CHAR *p = tmpBuf;
				while (*p == ' ' || *p == '\t')
					p++;

				// Copy until newline or end
				USIZE pos = 0;
				while (*p != '\0' && *p != '\n' && pos < buffer.Size() - 1)
					buffer.Data()[pos++] = *p++;

				// Trim trailing whitespace
				while (pos > 0 && (buffer.Data()[pos - 1] == ' ' || buffer.Data()[pos - 1] == '\t'))
					pos--;

				buffer.Data()[pos] = '\0';
				if (pos > 0)
					return Result<USIZE, Error>::Ok(pos);
			}
		}
	}
#endif

	return Result<USIZE, Error>::Err(Error(Error::None));
}

Result<USIZE, Error> Environment::GetHostname(Span<CHAR> buffer) noexcept
{
	// Try HOSTNAME environment variable first (works on Linux/Android)
	USIZE len = Environment::GetVariable("HOSTNAME", buffer);
	if (len > 0)
		return Result<USIZE, Error>::Ok(len);

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	// Fallback: read /etc/hostname
	{
		const CHAR *path = "/etc/hostname";
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
				return Result<USIZE, Error>::Ok(StringUtils::Length(buffer.Data()));
			}
		}
	}
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
	// Use sysctl to query kern.hostname
	// sysctl({CTL_KERN=1, KERN_HOSTNAME=10}, ...)
	{
		INT32 mib[2];
		mib[0] = 1;   // CTL_KERN
		mib[1] = 10;  // KERN_HOSTNAME
		USIZE slen = buffer.Size() - 1;
		Memory::Zero(buffer.Data(), buffer.Size());
		SSIZE ret = System::Call(SYS_SYSCTL, (USIZE)mib, 2, (USIZE)buffer.Data(), (USIZE)&slen, 0, 0);
		if (ret == 0 && buffer.Data()[0] != '\0')
			return Result<USIZE, Error>::Ok(StringUtils::Length(buffer.Data()));
	}
#elif defined(PLATFORM_SOLARIS)
	// Try utssys nodename field (works on illumos)
	{
		SolarisUtsname uts;
		Memory::Zero(&uts, sizeof(SolarisUtsname));
		SSIZE ret = System::Call(SYS_UTSSYS, (USIZE)&uts, 0, 0);
		if (ret == 0 && uts.Nodename[0] != '\0')
		{
			USIZE nodeLen = StringUtils::Length(uts.Nodename);
			StringUtils::Copy(buffer, Span<const CHAR>(uts.Nodename, nodeLen + 1));
			return Result<USIZE, Error>::Ok(nodeLen);
		}
	}

	// Fallback: read /etc/nodename (present on Oracle Solaris 11.4)
	// then /etc/hostname (present on some illumos distributions)
	{
		const CHAR *paths[] = { "/etc/nodename", "/etc/hostname" };
		for (USIZE p = 0; p < 2; p++)
		{
			SSIZE fd = System::Call(SYS_OPEN, (USIZE)paths[p], 0 /* O_RDONLY */, 0);
			if (fd < 0)
				fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)paths[p], 0, 0);
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
					if (buffer.Data()[0] != '\0')
						return Result<USIZE, Error>::Ok(StringUtils::Length(buffer.Data()));
				}
			}
		}
	}
#endif

	return Result<USIZE, Error>::Err(Error(Error::None));
}

USIZE Environment::GetArchitecture(Span<CHAR> buffer) noexcept
{
#if defined(ARCHITECTURE_X86_64)
	StringUtils::Copy(buffer, Span<const CHAR>("x86_64"));
#elif defined(ARCHITECTURE_I386)
	StringUtils::Copy(buffer, Span<const CHAR>("i386"));
#elif defined(ARCHITECTURE_AARCH64)
	StringUtils::Copy(buffer, Span<const CHAR>("aarch64"));
#elif defined(ARCHITECTURE_ARMV7A)
	StringUtils::Copy(buffer, Span<const CHAR>("armv7a"));
#elif defined(ARCHITECTURE_RISCV64)
	StringUtils::Copy(buffer, Span<const CHAR>("riscv64"));
#elif defined(ARCHITECTURE_RISCV32)
	StringUtils::Copy(buffer, Span<const CHAR>("riscv32"));
#elif defined(ARCHITECTURE_MIPS64)
	StringUtils::Copy(buffer, Span<const CHAR>("mips64"));
#else
	buffer.Data()[0] = '\0';
	return 0;
#endif
	return StringUtils::Length(buffer.Data());
}
