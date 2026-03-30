#include "platform/fs/directory_iterator.h"
#include "core/types/primitives.h"
#include "core/memory/memory.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/kernel/windows/ntdll.h"

// Helper to fill the entry from FILE_BOTH_DIR_INFORMATION
static VOID FillEntry(DirectoryEntry &entry, const FILE_BOTH_DIR_INFORMATION &data)
{
	// 1. Copy Name (FileNameLength is in bytes, divide by sizeof(WCHAR))
	UINT32 nameLen = data.FileNameLength / sizeof(WCHAR);
	if (nameLen > 255)
		nameLen = 255;
	for (UINT32 j = 0; j < nameLen; j++)
	{
		entry.Name[j] = data.FileName[j];
	}
	entry.Name[nameLen] = L'\0';

	// 2. Size
	entry.Size = data.EndOfFile.QuadPart;

	// 3. Attributes
	UINT32 attr = data.FileAttributes;
	entry.IsDirectory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	entry.IsHidden = (attr & FILE_ATTRIBUTE_HIDDEN) != 0;
	entry.IsSystem = (attr & FILE_ATTRIBUTE_SYSTEM) != 0;
	entry.IsReadOnly = (attr & FILE_ATTRIBUTE_READONLY) != 0;

	// 4. Timestamps
	entry.CreationTime = data.CreationTime.QuadPart;

	// 5. LastModifiedTime
	entry.LastModifiedTime = data.LastWriteTime.QuadPart;

	// 6. IsDrive
	entry.IsDrive = (entry.Name[1] == ':' && entry.Name[2] == L'\0');

	entry.Type = 3; // Default to Fixed
}

DirectoryIterator::DirectoryIterator()
		: handle((PVOID)-1), currentEntry{}, isFirst(true)
		{

		}

// DirectoryIterator factory
Result<DirectoryIterator, Error> DirectoryIterator::Create(PCWCHAR path)
{
	DirectoryIterator iter;

	// CASE: List Drives (Path is empty or nullptr)
	if (!path || path[0] == L'\0')
	{
		PROCESS_DEVICEMAP_INFORMATION processDeviceMapInfo;
		auto queryResult = NTDLL::ZwQueryInformationProcess(
			NTDLL::NtCurrentProcess(),
			ProcessDeviceMap,
			&processDeviceMapInfo.Query,
			sizeof(processDeviceMapInfo.Query),
			nullptr);

		if (!queryResult)
		{
			return Result<DirectoryIterator, Error>::Err(queryResult, Error::Fs_OpenFailed);
		}
		if (processDeviceMapInfo.Query.DriveMap != 0)
		{
			// Store the mask in the pointer itself
			iter.handle = (PVOID)(USIZE)processDeviceMapInfo.Query.DriveMap;
			iter.isFirst = true; // Flag to indicate we are in "Drive Mode"
			iter.isBitMaskMode = true;
		}
		return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
	}

	// Convert path to NT path and open directory handle
	UNICODE_STRING uniPath;
	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &uniPath, nullptr, nullptr);
	if (!pathResult)
		return Result<DirectoryIterator, Error>::Err(pathResult, Error::Fs_PathResolveFailed);

	OBJECT_ATTRIBUTES objAttr;
	InitializeObjectAttributes(&objAttr, &uniPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	IO_STATUS_BLOCK ioStatusBlock;
	auto openResult = NTDLL::ZwOpenFile(
		&iter.handle,
		FILE_LIST_DIRECTORY | SYNCHRONIZE,
		&objAttr,
		&ioStatusBlock,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);

	NTDLL::RtlFreeUnicodeString(&uniPath);

	if (!openResult)
	{
		iter.handle = (PVOID)-1;
		return Result<DirectoryIterator, Error>::Err(openResult, Error::Fs_OpenFailed);
	}

	// Query the isFirst entry
	alignas(alignof(FILE_BOTH_DIR_INFORMATION)) UINT8 buffer[sizeof(FILE_BOTH_DIR_INFORMATION) + 260 * sizeof(WCHAR)];
	Memory::Zero(buffer, sizeof(buffer));

	auto dirResult = NTDLL::ZwQueryDirectoryFile(
		iter.handle,
		nullptr,
		nullptr,
		nullptr,
		&ioStatusBlock,
		buffer,
		sizeof(buffer),
		FileBothDirectoryInformation,
		true,
		nullptr,
		true);

	if (dirResult)
	{
		const FILE_BOTH_DIR_INFORMATION &info = *(const FILE_BOTH_DIR_INFORMATION *)buffer;
		FillEntry(iter.currentEntry, info);
	}
	else
	{
		(VOID)NTDLL::ZwClose(iter.handle);
		iter.handle = (PVOID)-1;
		return Result<DirectoryIterator, Error>::Err(dirResult, Error::Fs_ReadFailed);
	}
	return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
}

// Move to next entry. Ok = has entry, Err = done or syscall failed.
Result<VOID, Error> DirectoryIterator::Next()
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	// --- MODE 1: Drive Bitmask Mode (isFirst is true and handle is small) ---
	// We treat handles < 0x1000000 as bitmasks (drives)
	if (isBitMaskMode)
	{
		USIZE mask = (USIZE)handle;

		if (mask == 0)
			return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

		// Query the process device map to get drive types
		PROCESS_DEVICEMAP_INFORMATION devMapInfo;
		Memory::Zero(&devMapInfo, sizeof(devMapInfo));
		auto devMapResult = NTDLL::ZwQueryInformationProcess(
			NTDLL::NtCurrentProcess(),
			ProcessDeviceMap,
			&devMapInfo.Query,
			sizeof(devMapInfo.Query),
			nullptr);

		// Find the next set bit
		for (INT32 i = 0; i < 26; i++)
		{
			if (mask & (1 << i))
			{
				// Found a drive! Format it as "X:\"
				currentEntry.Name[0] = (WCHAR)(L'A' + i);
				currentEntry.Name[1] = L':';
				currentEntry.Name[2] = L'\\';
				currentEntry.Name[3] = L'\0';

				currentEntry.IsDirectory = true;
				currentEntry.IsDrive = true;

				// DriveType[] uses Win32 drive type constants directly
				if (devMapResult)
					currentEntry.Type = (UINT32)devMapInfo.Query.DriveType[i];
				else
					currentEntry.Type = DRIVE_UNKNOWN;

				// Update mask for next time (remove the bit we just processed)
				mask &= ~(1 << i);
				handle = (PVOID)mask;
				isFirst = false;

				return Result<VOID, Error>::Ok();
			}
		}

		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);
	}

	// --- NORMAL MODE ---
	if (isFirst)
	{
		isFirst = false;
		return Result<VOID, Error>::Ok();
	}

	alignas(alignof(FILE_BOTH_DIR_INFORMATION)) UINT8 buffer[sizeof(FILE_BOTH_DIR_INFORMATION) + 260 * sizeof(WCHAR)];
	Memory::Zero(buffer, sizeof(buffer));

	auto dirResult = NTDLL::ZwQueryDirectoryFile(
		handle,
		nullptr,
		nullptr,
		nullptr,
		&ioStatusBlock,
		buffer,
		sizeof(buffer),
		FileBothDirectoryInformation,
		true,
		nullptr,
		false);

	if (dirResult)
	{
		const FILE_BOTH_DIR_INFORMATION &dirInfo = *(const FILE_BOTH_DIR_INFORMATION *)buffer;
		FillEntry(currentEntry, dirInfo);
		return Result<VOID, Error>::Ok();
	}

	return Result<VOID, Error>::Err(dirResult, Error::Fs_ReadFailed);
}

// Move constructor
DirectoryIterator::DirectoryIterator(DirectoryIterator &&other) noexcept
	: handle(other.handle), currentEntry(other.currentEntry), isFirst(other.isFirst), isBitMaskMode(other.isBitMaskMode)
{
	other.handle = (PVOID)-1;
}

DirectoryIterator &DirectoryIterator::operator=(DirectoryIterator &&other) noexcept
{
	if (this != &other)
	{
		if (IsValid())
			Close();
		handle = other.handle;
		currentEntry = other.currentEntry;
		isFirst = other.isFirst;
		isBitMaskMode = other.isBitMaskMode;
		other.handle = (PVOID)-1;
	}
	return *this;
}

VOID DirectoryIterator::Close()
{
	if (IsValid())
	{
		if (!isBitMaskMode)
			(VOID)NTDLL::ZwClose(handle);
		handle = (PVOID)-1;
	}
}

// Check if the iterator is valid
BOOL DirectoryIterator::IsValid() const
{
	// Windows returns (HANDLE)-1 (0xFFFFFFFF) on failure for FindFirstFile
	return handle != nullptr && handle != (PVOID)(SSIZE)-1;
}
