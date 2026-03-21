#pragma once

#include "platform/system/environment.h"
#include "platform/system/system_info.h"
#include "platform/system/machine_id.h"
#include "core/string/string.h"
#include "core/memory/memory.h"
#include "tests.h"

class EnvironmentTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Environment Tests...");

		RunTest(allPassed, &TestGetAgentPlatform, "GetAgentPlatform returns non-empty string");
		RunTest(allPassed, &TestGetAgentPlatformKnownValue, "GetAgentPlatform returns known platform");
		RunTest(allPassed, &TestGetOSVersion, "GetOSVersion returns non-empty string");
		RunTest(allPassed, &TestGetHostname, "GetHostname returns non-empty string");
		RunTest(allPassed, &TestGetArchitecture, "GetArchitecture returns known architecture");
		RunTest(allPassed, &TestGetMachineUUID, "GetMachineUUID returns valid UUID");
		RunTest(allPassed, &TestSystemInfoPopulated, "SystemInfo fields are populated");

		if (allPassed)
			LOG_INFO("All Environment tests passed!");
		else
			LOG_ERROR("Some Environment tests failed!");

		return allPassed;
	}

private:
	static BOOL TestGetAgentPlatform()
	{
		CHAR buffer[32];
		Memory::Zero(buffer, sizeof(buffer));

		USIZE len = Environment::GetAgentPlatform(Span<CHAR>(buffer, 31));

		if (len == 0)
		{
			LOG_ERROR("GetAgentPlatform returned length 0");
			return false;
		}

		if (buffer[0] == '\0')
		{
			LOG_ERROR("GetAgentPlatform returned empty string");
			return false;
		}

		LOG_INFO("  AgentPlatform: %s (len=%llu)", buffer, (UINT64)len);
		return true;
	}

	static BOOL TestGetAgentPlatformKnownValue()
	{
		CHAR buffer[32];
		Memory::Zero(buffer, sizeof(buffer));

		Environment::GetAgentPlatform(Span<CHAR>(buffer, 31));

		// Must be one of the known platform strings
		BOOL isKnown = StringUtils::Equals(buffer, "windows") ||
					   StringUtils::Equals(buffer, "linux") ||
					   StringUtils::Equals(buffer, "macos") ||
					   StringUtils::Equals(buffer, "android") ||
					   StringUtils::Equals(buffer, "ios") ||
					   StringUtils::Equals(buffer, "freebsd") ||
					   StringUtils::Equals(buffer, "solaris") ||
					   StringUtils::Equals(buffer, "uefi");

		if (!isKnown)
		{
			LOG_ERROR("GetAgentPlatform returned unknown value: %s", buffer);
			return false;
		}

		return true;
	}

	static BOOL TestGetOSVersion()
	{
		CHAR buffer[128];
		Memory::Zero(buffer, sizeof(buffer));

		auto result = Environment::GetOSVersion(Span<CHAR>(buffer, 127));

		if (!result)
		{
			LOG_ERROR("GetOSVersion failed");
			return false;
		}

		USIZE len = result.Value();

		// On Windows, should start with "Windows"
#if defined(PLATFORM_WINDOWS)
		if (buffer[0] != 'W' || buffer[1] != 'i' || buffer[2] != 'n')
		{
			LOG_ERROR("GetOSVersion on Windows doesn't start with 'Windows': %s", buffer);
			return false;
		}
#endif

		// On Linux/Android, should start with "Linux" (from uname sysname)
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
		if (buffer[0] != 'L' || buffer[1] != 'i' || buffer[2] != 'n')
		{
			LOG_ERROR("GetOSVersion on Linux doesn't start with 'Linux': %s", buffer);
			return false;
		}
#endif

		LOG_INFO("  OSVersion: %s (len=%llu)", buffer, (UINT64)len);
		return true;
	}

	static BOOL TestGetHostname()
	{
		CHAR buffer[256];
		Memory::Zero(buffer, sizeof(buffer));

		auto result = Environment::GetHostname(Span<CHAR>(buffer, 255));

		if (!result)
		{
#if defined(PLATFORM_UEFI) || defined(PLATFORM_SOLARIS)
			// UEFI has no hostname concept; Solaris CI may lack /etc/nodename
			LOG_INFO("  GetHostname not available on this platform (expected)");
			return true;
#else
			LOG_ERROR("GetHostname failed");
			return false;
#endif
		}

		LOG_INFO("  Hostname: %s (len=%llu)", buffer, (UINT64)result.Value());
		return true;
	}

	static BOOL TestGetMachineUUID()
	{
		auto result = GetMachineUUID();

		if (!result)
		{
#if defined(PLATFORM_UEFI) || defined(PLATFORM_SOLARIS)
			// UEFI QEMU may lack SMBIOS; Solaris has no standard UUID source
			LOG_INFO("  GetMachineUUID not available on this platform (expected)");
			return true;
#else
			LOG_ERROR("GetMachineUUID failed");
			return false;
#endif
		}

		UUID &uuid = result.Value();
		if (uuid.GetMostSignificantBits() == 0 && uuid.GetLeastSignificantBits() == 0)
		{
			LOG_ERROR("GetMachineUUID returned nil UUID");
			return false;
		}

		CHAR uuidStr[37];
		auto toStrResult = uuid.ToString(Span<CHAR>(uuidStr));
		if (toStrResult.IsErr())
		{
			LOG_ERROR("UUID::ToString failed");
			return false;
		}
		LOG_INFO("  MachineUUID: %s", uuidStr);

		return true;
	}

	static BOOL TestGetArchitecture()
	{
		CHAR buffer[32];
		Memory::Zero(buffer, sizeof(buffer));

		USIZE len = Environment::GetArchitecture(Span<CHAR>(buffer, 31));

		if (len == 0)
		{
			LOG_ERROR("GetArchitecture returned length 0");
			return false;
		}

		// Must be one of the known architecture strings
		BOOL isKnown = StringUtils::Equals(buffer, "x86_64") ||
					   StringUtils::Equals(buffer, "i386") ||
					   StringUtils::Equals(buffer, "aarch64") ||
					   StringUtils::Equals(buffer, "armv7a") ||
					   StringUtils::Equals(buffer, "riscv64") ||
					   StringUtils::Equals(buffer, "riscv32") ||
					   StringUtils::Equals(buffer, "mips64");

		if (!isKnown)
		{
			LOG_ERROR("GetArchitecture returned unknown value: %s", buffer);
			return false;
		}

		LOG_INFO("  Architecture: %s (len=%llu)", buffer, (UINT64)len);
		return true;
	}

	static BOOL TestSystemInfoPopulated()
	{
		SystemInfo info;
		GetSystemInfo(&info);

		// Hostname may not be available on all platforms
		if (info.Hostname[0] == '\0')
		{
#if defined(PLATFORM_UEFI) || defined(PLATFORM_SOLARIS)
			LOG_INFO("  SystemInfo.Hostname empty (expected on this platform)");
#else
			LOG_ERROR("SystemInfo.Hostname is empty");
			return false;
#endif
		}

		if (info.Architecture[0] == '\0')
		{
			LOG_ERROR("SystemInfo.Architecture is empty");
			return false;
		}

		if (info.AgentPlatform[0] == '\0')
		{
			LOG_ERROR("SystemInfo.AgentPlatform is empty");
			return false;
		}

		if (info.OSVersion[0] == '\0')
		{
			LOG_ERROR("SystemInfo.OSVersion is empty");
			return false;
		}

		LOG_INFO("  SystemInfo: host=%s, arch=%s, agent_platform=%s, os_version=%s",
				 info.Hostname, info.Architecture, info.AgentPlatform, info.OSVersion);

		return true;
	}
};
