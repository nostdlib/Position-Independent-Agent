#include "platform/fs/directory_iterator.h"
#include "core/types/primitives.h"
#include "core/memory/memory.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/fs/windows/wpd.h"
#include "platform/console/logger.h"

// NTSTATUS values used by the iterator but absent from ntdll.h
constexpr UINT32 STATUS_NO_MORE_FILES_NT = 0x80000006u;	  // clean end of enumeration (verified on Win10)
constexpr UINT32 STATUS_NO_MORE_ENTRIES_NT = 0x8000001Au;	  // same meaning, some providers
constexpr UINT32 STATUS_NO_SUCH_FILE_NT = 0xC000000Fu;		  // clean end on empty directories (FAT & friends)
constexpr UINT32 STATUS_BUFFER_OVERFLOW_NT = 0x80000005u;	  // entry did not fit the query buffer
constexpr UINT32 STATUS_INFO_LENGTH_MISMATCH_NT = 0xC0000004u; // query buffer hopelessly small

// Clean end-of-enumeration statuses (any of them means EOF, not failure).
static BOOL IsEndOfEnumeration(UINT32 status)
{
	return status == STATUS_NO_MORE_FILES_NT || status == STATUS_NO_MORE_ENTRIES_NT || status == STATUS_NO_SUCH_FILE_NT;
}

/// Longest filename the grow-retry will accommodate (NTFS caps components at
/// 255 WCHARs; 512 leaves margin for odd reparse providers) — 64 KiB cap.
constexpr USIZE MAX_QUERY_BUFFER_SIZE = 64 * 1024;

// Non-null sentinel for "portable-device root phase was attempted and failed"
// so the lazily-started phase is tried at most once per root listing. Not
// constexpr: integer-to-pointer casts are not constant expressions.
static FORCE_INLINE PVOID WpdFailedSentinel()
{
	return (PVOID)1;
}

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

	// 6. IsDrive — drive roots are formatted "X:\"; ':' and '\' cannot appear in
	// an ordinary file name, so this cannot false-positive on real entries.
	entry.IsDrive = (entry.Name[1] == L':' && entry.Name[2] == L'\\' && entry.Name[3] == L'\0');

	// 7. VolumeSerial (files and directories never carry one)
	entry.VolumeSerial = 0;

	entry.Type = 3; // Default to Fixed
}

// Queries the volume serial number for a drive root (e.g. L"C:\\") via
// FileFsVolumeInformation. Best-effort: returns 0 when the volume cannot be
// opened or queried (no media, BitLocker-locked, ...).
static UINT64 QueryVolumeSerial(PCWCHAR driveRoot)
{
	// 1. Resolve the drive root to an NT path (\??\X:\) through the DOS device map
	UNICODE_STRING uniPath;
	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(driveRoot, &uniPath, nullptr, nullptr);
	if (!pathResult)
		return 0;

	OBJECT_ATTRIBUTES objAttr;
	InitializeObjectAttributes(&objAttr, &uniPath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	// 2. Open the volume root. FILE_READ_ATTRIBUTES is sufficient for FileFsVolumeInformation.
	PVOID volumeHandle = nullptr;
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	auto openResult = NTDLL::ZwOpenFile(
		&volumeHandle,
		FILE_READ_ATTRIBUTES | SYNCHRONIZE,
		&objAttr,
		&ioStatusBlock,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);

	NTDLL::RtlFreeUnicodeString(&uniPath);

	if (!openResult)
		return 0;

	// 3. Query the serial. VolumeLabel is variable length, so over-allocate for it;
	//    only VolumeSerialNumber (at a fixed offset) is consumed.
	alignas(alignof(FILE_FS_VOLUME_INFORMATION)) UINT8 buffer[sizeof(FILE_FS_VOLUME_INFORMATION) + 32 * sizeof(WCHAR)];
	Memory::Zero(buffer, sizeof(buffer));

	auto queryResult = NTDLL::ZwQueryVolumeInformationFile(
		volumeHandle,
		&ioStatusBlock,
		buffer,
		sizeof(buffer),
		FileFsVolumeInformation);

	(VOID)NTDLL::ZwClose(volumeHandle);

	// STATUS_BUFFER_OVERFLOW (0x80000005) means the variable-length label did
	// not fit; the kernel fills the fixed header first, so VolumeSerialNumber
	// is still valid. Only other failures yield "unknown".
	if (!queryResult && (UINT32)queryResult.Error().Code != 0x80000005u)
		return 0;

	const FILE_FS_VOLUME_INFORMATION &info = *(const FILE_FS_VOLUME_INFORMATION *)buffer;
	return (UINT64)info.VolumeSerialNumber;
}

// Queries exactly one directory entry (ReturnSingleEntry=true) into `buffer`.
// When the entry does not fit (STATUS_BUFFER_OVERFLOW /
// STATUS_INFO_LENGTH_MISMATCH — a filename longer than the buffer), retries
// with a doubled heap buffer up to MAX_QUERY_BUFFER_SIZE instead of failing:
// an over-long name must end up IN the listing, not silently end it.
// On success returns Ok with the FILE_BOTH_DIR_INFORMATION pointer (the stack
// buffer or the heap buffer — caller deletes *heapBuffer when non-null). On
// failure returns the raw ZwQueryDirectoryFile error; a heap buffer taken
// before the failure is still returned for cleanup.
static Result<const FILE_BOTH_DIR_INFORMATION *, Error> QuerySingleEntry(
	PVOID dirHandle, IO_STATUS_BLOCK &ioStatusBlock, BOOL restartScan,
	UINT8 *buffer, USIZE bufferSize, UINT8 **heapBuffer)
{
	*heapBuffer = nullptr;
	UINT8 *current = buffer;
	USIZE currentSize = bufferSize;
	Error lastError(Error::Fs_ReadFailed);

	while (true)
	{
		Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));
		auto dirResult = NTDLL::ZwQueryDirectoryFile(
			dirHandle,
			nullptr,
			nullptr,
			nullptr,
			&ioStatusBlock,
			current,
			currentSize,
			FileBothDirectoryInformation,
			true,
			nullptr,
			restartScan);

		if (dirResult)
			return Result<const FILE_BOTH_DIR_INFORMATION *, Error>::Ok((const FILE_BOTH_DIR_INFORMATION *)current);

		lastError = dirResult.Error();
		UINT32 status = (UINT32)lastError.Code;
		if ((status == STATUS_BUFFER_OVERFLOW_NT || status == STATUS_INFO_LENGTH_MISMATCH_NT)
			&& currentSize < MAX_QUERY_BUFFER_SIZE)
		{
			// Nothing was transferred — the enumeration position did not
			// move — so retrying with a larger buffer yields the same entry.
			USIZE nextSize = currentSize * 2;
			if (nextSize > MAX_QUERY_BUFFER_SIZE)
				nextSize = MAX_QUERY_BUFFER_SIZE;
			UINT8 *grown = new UINT8[nextSize];
			if (grown == nullptr)
				break; // out of memory — surface the original overflow status
			delete[] *heapBuffer; // previous growth attempt, if any
			*heapBuffer = grown;
			current = grown;
			currentSize = nextSize;
			continue;
		}
		break;
	}

	return Result<const FILE_BOTH_DIR_INFORMATION *, Error>::Err(lastError);
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
		else
		{
			// No logical drives: a VALID empty iterator (bitmask 0), not an
			// error — the caller must see "zero drives", and Next() reports
			// the clean-end sentinel immediately.
			iter.handle = (PVOID)(USIZE)0;
			iter.isFirst = true;
			iter.isBitMaskMode = true;
		}
		return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
	}

	// CASE: Portable device pseudo-path (::mtp-...) — route before NT path
	// conversion; anything else falls through to the NT layer unchanged.
	UINT64 wpdToken = 0;
	const WCHAR *wpdSubpath = nullptr;
	USIZE wpdSubpathLen = 0;
	if (WPD::TryParsePath(path, wpdToken, wpdSubpath, wpdSubpathLen))
	{
		auto wpdResult = WPD::BeginObjectEnumeration(path);
		if (!wpdResult)
			return Result<DirectoryIterator, Error>::Err(wpdResult, Error::Fs_OpenFailed);
		iter.isWpdMode = true;
		iter.wpdState = wpdResult.Value();
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

	// Query the first entry (RestartScan=true). An end-of-enumeration status
	// here means the directory is EMPTY — return a valid iterator whose first
	// Next() reports the clean-end sentinel; a genuinely empty listing must
	// not be reported as an error.
	alignas(alignof(FILE_BOTH_DIR_INFORMATION)) UINT8 buffer[sizeof(FILE_BOTH_DIR_INFORMATION) + 260 * sizeof(WCHAR)];
	UINT8 *heapBuffer = nullptr;
	auto entryResult = QuerySingleEntry(iter.handle, ioStatusBlock, true, buffer, sizeof(buffer), &heapBuffer);

	if (entryResult)
	{
		FillEntry(iter.currentEntry, *entryResult.Value());
	}
	else
	{
		UINT32 status = (UINT32)entryResult.Error().Code;
		if (IsEndOfEnumeration(status))
		{
			delete[] heapBuffer;
			// Empty directory: keep the handle, mark the pre-read entry as
			// consumed so the first Next() queries and hits the sentinel.
			iter.isFirst = false;
			return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
		}
		delete[] heapBuffer;
		(VOID)NTDLL::ZwClose(iter.handle);
		iter.handle = (PVOID)-1;
		return Result<DirectoryIterator, Error>::Err(entryResult.Error(), Error::Fs_ReadFailed);
	}
	delete[] heapBuffer;
	return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
}

// Move to next entry. Ok = has entry; Err with Code == Fs_NoMoreEntries is the
// CLEAN end of enumeration; any other Err is a real failure (device removed,
// access revoked mid-scan, ...) and must be surfaced, never treated as EOF.
Result<VOID, Error> DirectoryIterator::Next()
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	// --- MODE 0: Portable Device Mode (::mtp- pseudo-root) ---
	if (isWpdMode)
	{
		auto entryResult = WPD::NextEntry((WpdIteratorState *)wpdState, currentEntry);
		if (entryResult && entryResult.Value())
			return Result<VOID, Error>::Ok();
		if (entryResult)
			return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries); // clean end
		return Result<VOID, Error>::Err(entryResult.Error(), Error::Fs_ReadFailed);
	}

	// --- MODE 1: Drive Bitmask Mode (isFirst is true and handle is small) ---
	// We treat handles < 0x1000000 as bitmasks (drives)
	if (isBitMaskMode)
	{
		USIZE mask = (USIZE)handle;

		// Drive bitmask exhausted: lazily start the portable-device phase
		// ONCE, then drain it. Failures degrade to a clean end — the drives
		// already emitted must not turn the listing into an error.
		if (mask == 0)
		{
			if (wpdState == nullptr)
			{
				auto rootResult = WPD::BeginRootEnumeration();
				if (rootResult)
					wpdState = rootResult.Value();
				else
				{
					wpdState = WpdFailedSentinel();
					LOG_WARNING("Portable device enumeration unavailable");
				}
			}
			if (wpdState == WpdFailedSentinel())
				return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries);

			auto entryResult = WPD::NextEntry((WpdIteratorState *)wpdState, currentEntry);
			if (entryResult && entryResult.Value())
				return Result<VOID, Error>::Ok();
			if (entryResult)
				return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries); // clean end of devices
			// Hard failure mid-device-list: end cleanly, never re-enter.
			LOG_WARNING("Portable device enumeration failed mid-listing");
			WPD::EndEnumeration((WpdIteratorState *)wpdState);
			wpdState = WpdFailedSentinel();
			return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries);
		}

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

				// Fetch the volume serial only when the device map positively
				// identifies the drive as non-remote: opening an unreachable
				// share's root can block for the redirector timeout (seconds per
				// drive) inside this synchronous call. A degraded device-map
				// query (Type == DRIVE_UNKNOWN) still queries: that value means
				// "type unknown", not "unopenable volume".
				if (devMapResult && devMapInfo.Query.DriveType[i] == DRIVE_REMOTE)
					currentEntry.VolumeSerial = 0;
				else
					currentEntry.VolumeSerial = QueryVolumeSerial(currentEntry.Name);

				// Update mask for next time (remove the bit we just processed)
				mask &= ~(1 << i);
				handle = (PVOID)mask;
				isFirst = false;

				return Result<VOID, Error>::Ok();
			}
		}

		return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries); // clean end of drive list
	}

	// --- NORMAL MODE ---
	if (isFirst)
	{
		isFirst = false;
		return Result<VOID, Error>::Ok();
	}

	alignas(alignof(FILE_BOTH_DIR_INFORMATION)) UINT8 buffer[sizeof(FILE_BOTH_DIR_INFORMATION) + 260 * sizeof(WCHAR)];
	UINT8 *heapBuffer = nullptr;
	auto entryResult = QuerySingleEntry(handle, ioStatusBlock, false, buffer, sizeof(buffer), &heapBuffer);

	if (entryResult)
	{
		FillEntry(currentEntry, *entryResult.Value());
		delete[] heapBuffer;
		return Result<VOID, Error>::Ok();
	}
	delete[] heapBuffer;

	UINT32 status = (UINT32)entryResult.Error().Code;
	if (IsEndOfEnumeration(status))
		return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries); // clean end of directory

	// Real failure (volume dismounted, device removed, access revoked, ...):
	// keep the OS status in the error so the operator can see WHY the listing
	// stopped — never report this as a normal end of enumeration.
	return Result<VOID, Error>::Err(entryResult.Error(), Error::Fs_ReadFailed);
}

// Move constructor
DirectoryIterator::DirectoryIterator(DirectoryIterator &&other) noexcept
	: handle(other.handle), currentEntry(other.currentEntry), isFirst(other.isFirst), isBitMaskMode(other.isBitMaskMode), isWpdMode(other.isWpdMode), wpdState(other.wpdState)
{
	other.handle = (PVOID)-1;
	other.isWpdMode = false;
	other.wpdState = nullptr;
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
		isWpdMode = other.isWpdMode;
		wpdState = other.wpdState;
		other.handle = (PVOID)-1;
		other.isWpdMode = false;
		other.wpdState = nullptr;
	}
	return *this;
}

VOID DirectoryIterator::Close()
{
	// Portable-device state (object phase or the lazily-started root phase)
	// must be released regardless of the NT handle's validity.
	if (wpdState != nullptr)
	{
		if (wpdState != WpdFailedSentinel())
			WPD::EndEnumeration((WpdIteratorState *)wpdState);
		wpdState = nullptr;
		isWpdMode = false;
	}
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
	// Portable-device iterators carry no NT handle; the state is the validity.
	if (isWpdMode)
		return wpdState != nullptr && wpdState != WpdFailedSentinel();
	// Bitmask iterators: a zero mask is the valid "no drives" state, but the
	// closed/moved sentinel must still read invalid.
	if (isBitMaskMode)
		return handle != (PVOID)(SSIZE)-1;
	// Windows returns (HANDLE)-1 (0xFFFFFFFF) on failure for FindFirstFile
	return handle != nullptr && handle != (PVOID)(SSIZE)-1;
}
