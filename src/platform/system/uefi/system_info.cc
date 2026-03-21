#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

VOID GetSystemInfo(SystemInfo *info)
{
	Memory::Zero(info, sizeof(SystemInfo));

	// Machine UUID
	auto uuidResult = GetMachineUUID();
	if (uuidResult.IsOk())
		info->MachineUUID = uuidResult.Value();
	else
		LOG_ERROR("Failed to retrieve machine UUID");

	// UEFI has no hostname concept
	StringUtils::Copy(Span<CHAR>(info->Hostname, 255), Span<const CHAR>("unknown"));

	// CPU architecture (compile-time)
#if defined(ARCHITECTURE_X86_64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("x86_64"));
#elif defined(ARCHITECTURE_AARCH64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("aarch64"));
#else
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("unknown"));
#endif

	// Agent platform (compile-time)
	Environment::GetAgentPlatform(Span<CHAR>(info->AgentPlatform, 31));

	// Runtime OS version
	Environment::GetOSVersion(Span<CHAR>(info->OSVersion, 127));
}
