#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/fs/file.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

VOID GetSystemInfo(SystemInfo *info)
{
	Memory::Zero(info, sizeof(SystemInfo));

	// Machine UUID from /etc/machine-id or boot_id
	auto uuidResult = GetMachineUUID();
	if (uuidResult.IsOk())
		info->MachineUUID = uuidResult.Value();
	else
		LOG_ERROR("Failed to retrieve machine UUID");

	// Hostname from HOSTNAME environment variable
	USIZE len = Environment::GetVariable("HOSTNAME", Span<CHAR>(info->Hostname, 255));

	// Fallback: read /etc/hostname
	if (len == 0)
	{
		auto openResult = File::Open(L"/etc/hostname", File::ModeRead);
		if (openResult)
		{
			File &file = openResult.Value();
			auto readResult = file.Read(Span<UINT8>((UINT8 *)info->Hostname, 255));
			if (readResult && readResult.Value() > 0)
			{
				// Trim trailing newline
				len = readResult.Value();
				if (len > 0 && info->Hostname[len - 1] == '\n')
					info->Hostname[len - 1] = '\0';
			}
		}
	}

	if (info->Hostname[0] == '\0')
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
#elif defined(ARCHITECTURE_RISCV64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("riscv64"));
#elif defined(ARCHITECTURE_RISCV32)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("riscv32"));
#elif defined(ARCHITECTURE_MIPS64)
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("mips64"));
#else
	StringUtils::Copy(Span<CHAR>(info->Architecture, 31), Span<const CHAR>("unknown"));
#endif

	// Agent platform (compile-time)
	Environment::GetAgentPlatform(Span<CHAR>(info->AgentPlatform, 31));

	// Runtime OS version
	Environment::GetOSVersion(Span<CHAR>(info->OSVersion, 127));
}
