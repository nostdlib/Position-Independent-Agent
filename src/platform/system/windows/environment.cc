/**
 * environment.cc - Windows Environment Variable and Platform Implementation
 *
 * Accesses environment variables directly from the PEB environment block.
 * OS version is read from PEB OS version fields.
 * Position-independent, no .rdata dependencies.
 */

#include "platform/system/environment.h"
#include "platform/kernel/windows/peb.h"
#include "platform/kernel/windows/kernel32.h"
#include "platform/kernel/windows/ntdll.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/kernel/windows/pe.h"

// Extended RTL_USER_PROCESS_PARAMETERS with Environment field
// The standard definition in peb.h doesn't include all fields
struct RTL_USER_PROCESS_PARAMETERS_EX
{
	UINT32 MaximumLength;
	UINT32 Length;
	UINT32 Flags;
	UINT32 DebugFlags;
	PVOID ConsoleHandle;
	UINT32 ConsoleFlags;
	PVOID StandardInput;
	PVOID StandardOutput;
	PVOID StandardError;
	UNICODE_STRING CurrentDirectory_DosPath;
	PVOID CurrentDirectory_Handle;
	UNICODE_STRING DllPath;
	UNICODE_STRING ImagePathName;
	UNICODE_STRING CommandLine;
	PWCHAR Environment; // Pointer to environment block
};

// Helper to compare wide string with narrow string (case-insensitive for first part)
static BOOL CompareEnvName(const WCHAR *wide, const CHAR *narrow) noexcept
{
	while (*narrow != '\0')
	{
		WCHAR w = *wide;
		CHAR n = *narrow;

		// Convert to uppercase for comparison
		if (w >= L'a' && w <= L'z')
			w -= 32;
		if (n >= 'a' && n <= 'z')
			n -= 32;

		if (w != (WCHAR)n)
		{
			return false;
		}
		wide++;
		narrow++;
	}

	// After name, should be '='
	return *wide == L'=';
}

USIZE Environment::GetVariable(const CHAR *name, Span<CHAR> buffer) noexcept
{
	if (name == nullptr || buffer.Size() == 0)
	{
		return 0;
	}

	PPEB peb = GetCurrentPEB();
	if (peb == nullptr || peb->ProcessParameters == nullptr)
	{
		return 0;
	}

	// Get extended process parameters with Environment field
	RTL_USER_PROCESS_PARAMETERS_EX *params = (RTL_USER_PROCESS_PARAMETERS_EX *)peb->ProcessParameters;
	PWCHAR envBlock = params->Environment;

	if (envBlock == nullptr)
	{
		return 0;
	}

	// Environment block is a sequence of null-terminated wide strings
	// Format: NAME=VALUE\0NAME=VALUE\0...\0\0
	while (*envBlock != L'\0')
	{
		// Check if this is the variable we're looking for
		if (CompareEnvName(envBlock, name))
		{
			// Find the '=' and skip past it
			const WCHAR *value = envBlock;
			while (*value != L'=' && *value != L'\0')
			{
				value++;
			}
			if (*value == L'=')
			{
				value++; 

				// Copy value to buffer (convert wide to narrow)
				USIZE len = 0;
				while (*value != L'\0' && len < buffer.Size() - 1)
				{
					// Simple wide to narrow conversion (ASCII only)
					buffer[len++] = (CHAR)*value++;
				}
				buffer[len] = '\0';
				return len;
			}
		}

		// Skip to next variable
		while (*envBlock != L'\0')
		{
			envBlock++;
		}
		envBlock++; // Skip the null terminator
	}

	// Variable not found
	buffer[0] = '\0';
	return 0;
}

USIZE Environment::GetAgentPlatform(Span<CHAR> buffer) noexcept
{
	StringUtils::Copy(buffer, Span<const CHAR>("windows"));
	return StringUtils::Length(buffer.Data());
}

Result<USIZE, Error> Environment::GetOSVersion(Span<CHAR> buffer) noexcept
{
	if (buffer.Size() == 0)
		return Result<USIZE, Error>::Err(Error(Error::None));

	// Read OS version fields from PEB at known architecture-specific offsets.
	// These fields (OSMajorVersion, OSMinorVersion, OSBuildNumber) are set by
	// the NT kernel during process creation and are always present.
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64)
	PPEB peb = GetCurrentPEB();
	UINT32 major = *(PUINT32)((PUINT8)peb + 0x118);
	UINT32 minor = *(PUINT32)((PUINT8)peb + 0x11C);
	UINT16 build = *(PUINT16)((PUINT8)peb + 0x120);
#elif defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A)
	PPEB peb = GetCurrentPEB();
	UINT32 major = *(PUINT32)((PUINT8)peb + 0xA4);
	UINT32 minor = *(PUINT32)((PUINT8)peb + 0xA8);
	UINT16 build = *(PUINT16)((PUINT8)peb + 0xAC);
#else
	UINT32 major = 0;
	UINT32 minor = 0;
	UINT16 build = 0;
#endif

	// Format: "Windows {Major}.{Minor} Build {Build}"
	CHAR numBuf[16];
	USIZE pos = 0;

	StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>("Windows "));
	pos += StringUtils::Length(buffer.Data() + pos);

	USIZE n = StringUtils::UIntToStr(major, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(numBuf, n + 1));
	pos += n;

	buffer.Data()[pos++] = '.';

	n = StringUtils::UIntToStr(minor, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(numBuf, n + 1));
	pos += n;

	StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(" Build "));
	pos += StringUtils::Length(buffer.Data() + pos);

	n = StringUtils::UIntToStr(build, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data() + pos, buffer.Size() - pos), Span<const CHAR>(numBuf, n + 1));
	pos += n;

	return Result<USIZE, Error>::Ok(pos);
}

Result<USIZE, Error> Environment::GetHostname(Span<CHAR> buffer) noexcept
{
	USIZE len = Environment::GetVariable("COMPUTERNAME", buffer);
	if (len > 0)
		return Result<USIZE, Error>::Ok(len);
	return Result<USIZE, Error>::Err(Error(Error::None));
}

Result<USIZE, Error> Environment::GetUsername(Span<CHAR> buffer) noexcept
{
	// USERNAME is populated in the PEB environment block at process creation.
	if (buffer.Size() > 0)
		buffer[0] = '\0';

	USIZE len = Environment::GetVariable("USERNAME", buffer);
	if (len > 0)
		return Result<USIZE, Error>::Ok(len);
	return Result<USIZE, Error>::Err(Error(Error::None));
}

// Map an IMAGE_FILE_MACHINE_* code to its OS-standard architecture name, or
// nullptr for unrecognized codes (caller falls back to the compile-time target).
static PCCHAR MachineToName(UINT16 machine) noexcept
{
	switch (machine)
	{
		case IMAGE_FILE_MACHINE_AMD64:
			return "amd64";
		case IMAGE_FILE_MACHINE_I386:
			return "i386";
		case IMAGE_FILE_MACHINE_ARM64:
			return "arm64";
		case IMAGE_FILE_MACHINE_ARMNT:
			return "arm";
		default:
			return nullptr;
	}
}

USIZE Environment::GetArchitecture(Span<CHAR> buffer) noexcept
{
	// Query the native host architecture so an emulated process (x86 or x64
	// under WOW64) still reports the real CPU. Machine names follow the
	// OS-standard identifiers (amd64/arm64/i386/arm). IsWow64Process2 is only
	// available on Windows 10 1511+; when it is missing (pre-1511), the legacy
	// IsWow64Process (XP SP2+) can at least tell x86-under-emulation from a
	// native x86 process: WOW64 only ever meant x64 there (the ARM64 emulator
	// post-dates this export), so TRUE implies an amd64 host.
	UINT16 processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
	UINT16 nativeMachine  = IMAGE_FILE_MACHINE_UNKNOWN;
	auto wow64Result = Kernel32::IsWow64Process2(NTDLL::NtCurrentProcess(), &processMachine, &nativeMachine);
	if (wow64Result)
	{
		PCCHAR name = MachineToName(nativeMachine);
		if (name != nullptr)
		{
			StringUtils::Copy(buffer, Span<const CHAR>(name, StringUtils::Length(name) + 1));
			return StringUtils::Length(buffer.Data());
		}
	}
	else
	{
		UINT32 wow64Process = 0;
		if (Kernel32::IsWow64Process(NTDLL::NtCurrentProcess(), &wow64Process) && wow64Process)
		{
			StringUtils::Copy(buffer, Span<const CHAR>("amd64"));
			return StringUtils::Length(buffer.Data());
		}
	}

	// Fallback: the architecture the agent was compiled for, reported with
	// the OS-standard identifiers so it matches the runtime-detected names.
#if defined(ARCHITECTURE_X86_64)
	StringUtils::Copy(buffer, Span<const CHAR>("amd64"));
#elif defined(ARCHITECTURE_I386)
	StringUtils::Copy(buffer, Span<const CHAR>("i386"));
#elif defined(ARCHITECTURE_AARCH64)
	StringUtils::Copy(buffer, Span<const CHAR>("arm64"));
#elif defined(ARCHITECTURE_ARMV7A)
	StringUtils::Copy(buffer, Span<const CHAR>("arm"));
#else
	buffer.Data()[0] = '\0';
	return 0;
#endif
	return StringUtils::Length(buffer.Data());
}

USIZE Environment::GetProcessArchitecture(Span<CHAR> buffer) noexcept
{
	// The PROCESS arch is the arch this binary was compiled for — a 32-bit agent under
	// WOW64 on an x64 machine is an i386 PROCESS (GetArchitecture reports the host CPU).
	// Reported with the C2's tag vocabulary so it joins the identity-tag filter directly;
	// the C2 compiles delivered injectors for exactly this arch (they deserialize into
	// this process).
#if defined(ARCHITECTURE_X86_64)
	StringUtils::Copy(buffer, Span<const CHAR>("x86_64"));
#elif defined(ARCHITECTURE_AARCH64)
	StringUtils::Copy(buffer, Span<const CHAR>("aarch64"));
#elif defined(ARCHITECTURE_I386)
	StringUtils::Copy(buffer, Span<const CHAR>("i386"));
#elif defined(ARCHITECTURE_ARMV7A)
	StringUtils::Copy(buffer, Span<const CHAR>("armv7a"));
#else
	buffer.Data()[0] = '\0';
	return 0;
#endif
	return StringUtils::Length(buffer.Data());
}
