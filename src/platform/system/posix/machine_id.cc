#include "platform/system/machine_id.h"
#include "platform/fs/file.h"
#include "platform/console/logger.h"
#include "core/string/string.h"
#include "core/memory/memory.h"

#if defined(PLATFORM_MACOS)
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#elif defined(PLATFORM_IOS)
#include "platform/kernel/ios/syscall.h"
#include "platform/kernel/ios/system.h"
#endif

static Result<UUID, Error> ReadMachineIdFromFile(const WCHAR *path, bool hasDashes)
{
	auto openResult = File::Open(path, File::ModeRead);
	if (!openResult)
		return Result<UUID, Error>::Err(Error(Error::None));

	File &file = openResult.Value();

	if (hasDashes)
	{
		// boot_id format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
		UINT8 buf[37]{};
		auto readResult = file.Read(Span<UINT8>(buf, 36));
		if (!readResult || readResult.Value() < 36)
			return Result<UUID, Error>::Err(Error(Error::None));

		return UUID::FromString(Span<const CHAR>((const CHAR *)buf, 36));
	}

	// machine-id format: 32 hex characters, no dashes
	UINT8 buf[33]{};
	auto readResult = file.Read(Span<UINT8>(buf, 32));
	if (!readResult || readResult.Value() < 32)
		return Result<UUID, Error>::Err(Error(Error::None));

	// Format as 8-4-4-4-12 for UUID::FromString by inserting dashes.
	CHAR uuidStr[37]{};
	INT32 src = 0;
	INT32 dst = 0;
	for (INT32 i = 0; i < 32; i++)
	{
		if (i == 8 || i == 12 || i == 16 || i == 20)
			uuidStr[dst++] = '-';
		uuidStr[dst++] = (CHAR)buf[src++];
	}
	uuidStr[dst] = '\0';

	return UUID::FromString(Span<const CHAR>(uuidStr, 36));
}

Result<UUID, Error> GetMachineUUID()
{
	// Try /etc/machine-id first (systemd, 32-char hex, constant across reboots)
	auto result = ReadMachineIdFromFile(L"/etc/machine-id", false);
	if (result)
		return result;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	// Fall back to /proc/sys/kernel/random/boot_id (UUID format with dashes).
	// Available on all Linux kernels. Note: boot_id changes on each reboot.
	result = ReadMachineIdFromFile(L"/proc/sys/kernel/random/boot_id", true);
	if (result)
		return result;
#endif

#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	// Use sysctl kern.uuid — the IOPlatformUUID (hardware, persists across reinstalls).
	// kern.uuid has a dynamically assigned OID (OID_AUTO), so we first translate the
	// name to a MIB path via the sysctl name2oid handler ({0, 3}), then query the value.
	{
		// Step 1: Translate "kern.uuid" to MIB path
		INT32 name2oid[2];
		name2oid[0] = 0;  // CTL_SYSCTL
		name2oid[1] = 3;  // SYSCTL_NAME2OID
		INT32 mib[8];
		USIZE mibLen = sizeof(mib);
		const CHAR *kernUuid = "kern.uuid";
		SSIZE ret = System::Call(SYS_SYSCTL, (USIZE)name2oid, 2,
								(USIZE)mib, (USIZE)&mibLen,
								(USIZE)kernUuid, 9);
		if (ret == 0 && mibLen > 0)
		{
			// Step 2: Query the actual UUID string using the resolved MIB
			CHAR uuidStr[40];
			USIZE uuidLen = sizeof(uuidStr) - 1;
			Memory::Zero(uuidStr, sizeof(uuidStr));
			USIZE mibCount = mibLen / sizeof(INT32);
			ret = System::Call(SYS_SYSCTL, (USIZE)mib, mibCount,
							  (USIZE)uuidStr, (USIZE)&uuidLen, 0, 0);
			if (ret == 0 && uuidStr[0] != '\0')
			{
				auto parseResult = UUID::FromString(Span<const CHAR>(uuidStr, StringUtils::Length(uuidStr)));
				if (parseResult)
					return parseResult;
			}
		}
	}
#endif

#if defined(PLATFORM_SOLARIS)
	// Solaris has no standard persistent machine UUID file.
	// /etc/hostid contains only 8 hex chars (not a full UUID).
	// Accept that machine UUID may not be available on Solaris.
#endif

	LOG_ERROR("Failed to retrieve machine UUID from OS");
	return Result<UUID, Error>::Err(Error(Error::None));
}
