#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/kernel/windows/peb.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

/// @brief Reads the runtime Windows version from the PEB and formats it into
/// the provided buffer as "Windows {Major}.{Minor} Build {Build}".
/// @details The PEB contains OS version fields at architecture-specific offsets:
/// - x86_64/aarch64: OSMajorVersion at 0x118, OSMinorVersion at 0x11C, OSBuildNumber at 0x120
/// - i386: OSMajorVersion at 0xA4, OSMinorVersion at 0xA8, OSBuildNumber at 0xAC
static VOID GetWindowsVersionFromPEB(Span<CHAR> buffer)
{
	PPEB peb = GetCurrentPEB();

#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64)
	UINT32 major = *(PUINT32)((PUINT8)peb + 0x118);
	UINT32 minor = *(PUINT32)((PUINT8)peb + 0x11C);
	UINT16 build = *(PUINT16)((PUINT8)peb + 0x120);
#elif defined(ARCHITECTURE_I386)
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

	StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>("Windows "));
	pos += StringUtils::Length(buffer.Data + pos);

	USIZE n = StringUtils::UIntToStr(major, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(numBuf, n + 1));
	pos += n;

	buffer.Data[pos++] = '.';

	n = StringUtils::UIntToStr(minor, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(numBuf, n + 1));
	pos += n;

	StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(" Build "));
	pos += StringUtils::Length(buffer.Data + pos);

	n = StringUtils::UIntToStr(build, Span<CHAR>(numBuf, 16));
	StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(numBuf, n + 1));
}

VOID GetSystemInfo(SystemInfo *info)
{
	Memory::Zero(info, sizeof(SystemInfo));

	// Machine UUID from SMBIOS
	auto uuidResult = GetMachineUUID();
	if (uuidResult.IsOk())
		info->MachineUUID = uuidResult.Value();
	else
		LOG_ERROR("Failed to retrieve machine UUID (error: %e)", uuidResult.Error());

	// Hostname from COMPUTERNAME environment variable
	USIZE len = Environment::GetVariable("COMPUTERNAME", Span<CHAR>(info->Hostname, 255));
	if (len == 0)
	{
		StringUtils::Copy(Span<CHAR>(info->Hostname, 255), Span<const CHAR>("unknown"));
	}

	// CPU architecture (compile-time)
#if defined(ARCHITECTURE_X86_64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("x86_64"));
#elif defined(ARCHITECTURE_I386)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("i386"));
#elif defined(ARCHITECTURE_AARCH64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("aarch64"));
#elif defined(ARCHITECTURE_ARMV7A)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("armv7a"));
#else
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("unknown"));
#endif

	// Agent platform (compile-time)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("windows"));

	// Runtime OS version from PEB
	GetWindowsVersionFromPEB(Span<CHAR>(info->Platform, 127));
}
