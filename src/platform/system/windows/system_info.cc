#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

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
	Environment::GetAgentPlatform(Span<CHAR>(info->AgentPlatform, 31));

	// Runtime OS version
	Environment::GetOSVersion(Span<CHAR>(info->OSVersion, 127));
}
