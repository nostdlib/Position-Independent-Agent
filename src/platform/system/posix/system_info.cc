#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "platform/system/environment.h"
#include "platform/fs/file.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#endif

/// @brief Populates the runtime OS version string.
/// @details On Linux/Android, uses the uname syscall to get the kernel
/// release and formats it as "{sysname} {release}" (e.g. "Linux 6.1.0").
/// On other POSIX platforms, reads /proc/version or /etc/os-release as a
/// best-effort fallback.
static VOID GetPlatformVersion(Span<CHAR> buffer)
{
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	Utsname uts;
	Memory::Zero(&uts, sizeof(Utsname));
	SSIZE ret = System::Call(SYS_UNAME, (USIZE)&uts);
	if (ret == 0)
	{
		// Format: "{sysname} {release}" e.g. "Linux 6.1.0"
		USIZE sysLen = StringUtils::Length(uts.Sysname);
		USIZE relLen = StringUtils::Length(uts.Release);
		USIZE pos = 0;

		StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(uts.Sysname, sysLen + 1));
		pos += sysLen;

		buffer.Data[pos++] = ' ';

		StringUtils::Copy(Span<CHAR>(buffer.Data + pos, buffer.Size - pos), Span<const CHAR>(uts.Release, relLen + 1));
		return;
	}
#endif

	// Fallback: try reading /proc/version (Linux if uname fails, FreeBSD with procfs)
	auto openResult = File::Open(L"/proc/version", File::ModeRead);
	if (openResult)
	{
		File &file = openResult.Value();
		auto readResult = file.Read(Span<UINT8>((UINT8 *)buffer.Data, buffer.Size - 1));
		if (readResult && readResult.Value() > 0)
		{
			USIZE len = readResult.Value();
			// Trim trailing newline
			if (len > 0 && buffer.Data[len - 1] == '\n')
				buffer.Data[len - 1] = '\0';
			else
				buffer.Data[len] = '\0';
			return;
		}
	}

	StringUtils::Copy(buffer, Span<const CHAR>("unknown"));
}

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
#if defined(PLATFORM_LINUX)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("linux"));
#elif defined(PLATFORM_MACOS)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("macos"));
#elif defined(PLATFORM_ANDROID)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("android"));
#elif defined(PLATFORM_IOS)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("ios"));
#elif defined(PLATFORM_FREEBSD)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("freebsd"));
#elif defined(PLATFORM_SOLARIS)
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("solaris"));
#else
	StringUtils::Copy(Span<CHAR>(info->AgentPlatform, 31), Span<const CHAR>("unknown"));
#endif

	// Runtime OS version
	GetPlatformVersion(Span<CHAR>(info->Platform, 127));
}
