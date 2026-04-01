#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"

VOID GetSystemInfo(SystemInfo *info)
{
	Memory::Zero(info, sizeof(SystemInfo));

	auto uuidResult = GetMachineUUID();
	if (uuidResult)
		info->MachineUUID = uuidResult.Value();
	else
		LOG_ERROR("Failed to retrieve machine UUID");

	auto hostnameResult = Environment::GetHostname(Span<CHAR>(info->Hostname, 255));
	if (!hostnameResult)
		LOG_ERROR("Failed to retrieve hostname");

	Environment::GetArchitecture(Span<CHAR>(info->Architecture, 31));
	Environment::GetAgentPlatform(Span<CHAR>(info->AgentPlatform, 31));

	auto osVersionResult = Environment::GetOSVersion(Span<CHAR>(info->OSVersion, 127));
	if (!osVersionResult)
		LOG_ERROR("Failed to retrieve OS version");
}
