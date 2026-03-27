#include "platform/system/machine_id.h"
#include "core/memory/memory.h"
#include "platform/console/logger.h"
#include "platform/kernel/windows/ntdll.h"

Result<UUID, Error> GetMachineUUID()
{
	// First query to determine required buffer size.
	// Build the request header for SMBIOS raw table retrieval.
	SYSTEM_FIRMWARE_TABLE_INFORMATION request{};
	request.ProviderSignature = RSMB_PROVIDER_SIGNATURE;
	request.Action = SystemFirmwareTable_Get;
	request.TableID = 0;
	request.TableBufferLength = 0;

	UINT32 requiredSize = 0;
	(VOID)NTDLL::ZwQuerySystemInformation(
		SystemFirmwareTableInformation,
		&request,
		sizeof(SYSTEM_FIRMWARE_TABLE_INFORMATION),
		&requiredSize);

	if (requiredSize == 0)
	{
		LOG_ERROR("Failed to query SMBIOS firmware table size");
		return Result<UUID, Error>::Err(Error(Error::None));
	}

	// Allocate buffer and query the actual firmware table data.
	PUINT8 buffer = new UINT8[requiredSize];
	auto firmwareInfo = (PSYSTEM_FIRMWARE_TABLE_INFORMATION)buffer;
	firmwareInfo->ProviderSignature = RSMB_PROVIDER_SIGNATURE;
	firmwareInfo->Action = SystemFirmwareTable_Get;
	firmwareInfo->TableID = 0;
	firmwareInfo->TableBufferLength = requiredSize - (UINT32)__builtin_offsetof(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);

	auto queryResult = NTDLL::ZwQuerySystemInformation(
		SystemFirmwareTableInformation,
		firmwareInfo,
		requiredSize,
		&requiredSize);

	if (!queryResult.IsOk() || !NT_SUCCESS(queryResult.Value()))
	{
		LOG_ERROR("Failed to query SMBIOS firmware table data");
		delete[] buffer;
		return Result<UUID, Error>::Err(Error(Error::None));
	}

	// Parse the raw SMBIOS data to find Type 1 (System Information).
	auto rawSmbios = (PRAW_SMBIOS_DATA)firmwareInfo->TableBuffer;
	PUINT8 tableData = rawSmbios->SMBIOSTableData;
	UINT32 tableLength = rawSmbios->Length;
	PUINT8 tableEnd = tableData + tableLength;

	UUID uuid{};
	BOOL found = false;

	while (tableData < tableEnd)
	{
		auto header = (PSMBIOS_HEADER)tableData;

		if (tableData + header->Length > tableEnd)
			break;

		// Type 1 = System Information, Length >= 0x19 means UUID field is present.
		if (header->Type == 1 && header->Length >= 0x19)
		{
			auto systemInfo = (PSMBIOS_TYPE1_SYSTEM_INFORMATION)tableData;

			// Check for all-FF (not set) or all-00 (not present).
			BOOL allZero = true;
			BOOL allFF = true;
			for (INT32 i = 0; i < 16; i++)
			{
				if (systemInfo->UUID[i] != 0x00) allZero = false;
				if (systemInfo->UUID[i] != 0xFF) allFF = false;
			}

			if (!allZero && !allFF)
			{
				// SMBIOS UUID is stored in mixed-endian format per spec.
				// First 3 fields are little-endian, remaining 8 bytes are big-endian.
				// Convert to standard RFC 9562 network byte order.
				UINT8 uuidBytes[16];

				// TimeLow (4 bytes, little-endian -> big-endian)
				uuidBytes[0] = systemInfo->UUID[3];
				uuidBytes[1] = systemInfo->UUID[2];
				uuidBytes[2] = systemInfo->UUID[1];
				uuidBytes[3] = systemInfo->UUID[0];

				// TimeMid (2 bytes, little-endian -> big-endian)
				uuidBytes[4] = systemInfo->UUID[5];
				uuidBytes[5] = systemInfo->UUID[4];

				// TimeHiAndVersion (2 bytes, little-endian -> big-endian)
				uuidBytes[6] = systemInfo->UUID[7];
				uuidBytes[7] = systemInfo->UUID[6];

				// Remaining 8 bytes are already in big-endian order.
				for (INT32 i = 8; i < 16; i++)
					uuidBytes[i] = systemInfo->UUID[i];

				uuid = UUID(Span<const UINT8, 16>(uuidBytes));
				found = true;
				break;
			}
		}

		// Skip past the formatted area to the string table.
		PUINT8 stringTable = tableData + header->Length;

		// The string table is terminated by a double null (0x00, 0x00).
		while (stringTable < tableEnd - 1 && !(stringTable[0] == 0x00 && stringTable[1] == 0x00))
			stringTable++;

		// Advance past the double null terminator to the next structure.
		tableData = stringTable + 2;
	}

	delete[] buffer;

	if (!found)
	{
		LOG_ERROR("SMBIOS Type 1 (System Information) UUID not found");
		return Result<UUID, Error>::Err(Error(Error::None));
	}

	return Result<UUID, Error>::Ok(uuid);
}
