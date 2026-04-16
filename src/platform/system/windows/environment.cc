/**
 * environment.cc - Windows Environment Variable and Platform Implementation
 *
 * Accesses environment variables directly from the PEB environment block.
 * OS version is read from PEB OS version fields.
 * Position-independent, no .rdata dependencies.
 */

#include "platform/system/environment.h"
#include "platform/kernel/windows/peb.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

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

	// Get PEB
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
				value++; // Skip the '='

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

USIZE Environment::GetCommandLineValue(const CHAR *flag, Span<CHAR> buffer) noexcept
{
	if (flag == nullptr || buffer.Size() == 0)
		return 0;

	PPEB peb = GetCurrentPEB();
	if (peb == nullptr || peb->ProcessParameters == nullptr)
		return 0;

	// Access the CommandLine UNICODE_STRING from the extended process parameters
	RTL_USER_PROCESS_PARAMETERS_EX *params = (RTL_USER_PROCESS_PARAMETERS_EX *)peb->ProcessParameters;
	UNICODE_STRING cmdLine = params->CommandLine;

	if (cmdLine.Buffer == nullptr || cmdLine.Length == 0)
		return 0;

	// Convert wide command line to narrow (ASCII-safe for flags and URLs)
	USIZE cmdLen = cmdLine.Length / sizeof(WCHAR);
	CHAR narrowCmd[2048];
	if (cmdLen >= sizeof(narrowCmd))
		cmdLen = sizeof(narrowCmd) - 1;

	for (USIZE i = 0; i < cmdLen; i++)
		narrowCmd[i] = (CHAR)cmdLine.Buffer[i];
	narrowCmd[cmdLen] = '\0';

	// Search for the flag
	USIZE flagLen = StringUtils::Length(flag);
	SSIZE idx = StringUtils::IndexOf(
		Span<const CHAR>(narrowCmd, cmdLen),
		Span<const CHAR>(flag, flagLen));

	if (idx < 0)
	{
		buffer[0] = '\0';
		return 0;
	}

	// Skip past the flag and any whitespace to get the value
	USIZE pos = (USIZE)idx + flagLen;
	while (pos < cmdLen && (narrowCmd[pos] == ' ' || narrowCmd[pos] == '\t'))
		pos++;

	if (pos >= cmdLen)
	{
		buffer[0] = '\0';
		return 0;
	}

	// Copy value until whitespace or end
	USIZE len = 0;
	while (pos + len < cmdLen && narrowCmd[pos + len] != ' ' && narrowCmd[pos + len] != '\t' && len < buffer.Size() - 1)
	{
		buffer[len] = narrowCmd[pos + len];
		len++;
	}
	buffer[len] = '\0';
	return len;
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
#else
	buffer.Data()[0] = '\0';
	return 0;
#endif
	return StringUtils::Length(buffer.Data());
}
