/**
 * directory_iterator.cc - UEFI DirectoryIterator Implementation
 *
 * Implements directory iteration using EFI_FILE_PROTOCOL.
 */

#include "platform/fs/directory_iterator.h"
#include "platform/fs/uefi/uefi_fs_helpers.h"
#include "core/memory/memory.h"

// =============================================================================
// DirectoryIterator Class Implementation
// =============================================================================

DirectoryIterator::DirectoryIterator()
	: handle(nullptr), currentEntry{}, isFirst(true)
{}

Result<DirectoryIterator, Error> DirectoryIterator::Create(PCWCHAR path)
{
	DirectoryIterator iter;
	(VOID) iter.isFirst; // Suppress unused warning - UEFI uses Read to iterate

	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<DirectoryIterator, Error>::Err(Error::Fs_OpenFailed);

	// Empty path means root directory - use the volume root handle directly
	// rather than calling Open() with L"" which some firmware doesn't support
	if (path == nullptr || path[0] == 0)
	{
		iter.handle = (PVOID)root;
		return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
	}

	EFI_FILE_PROTOCOL *dirHandle = OpenFileFromRoot(root, path, EFI_FILE_MODE_READ, 0);
	root->Close(root);

	if (dirHandle != nullptr)
	{
		iter.handle = (PVOID)dirHandle;
		return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
	}
	return Result<DirectoryIterator, Error>::Err(Error::Fs_OpenFailed);
}

DirectoryIterator::DirectoryIterator(DirectoryIterator &&other) noexcept
	: handle(other.handle), currentEntry(other.currentEntry), isFirst(other.isFirst)
{
	other.handle = nullptr;
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
		other.handle = nullptr;
	}
	return *this;
}

VOID DirectoryIterator::Close()
{
	if (IsValid())
	{
		EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)handle;
		fp->Close(fp);
		handle = nullptr;
	}
}

Result<VOID, Error> DirectoryIterator::Next()
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)handle;

	// Read one EFI_FILE_INFO. A failed read transfers nothing and does not
	// advance the directory position, so EFI_BUFFER_TOO_SMALL (name longer
	// than the buffer) is retried with a heap buffer of the reported size —
	// an over-long name must land IN the listing, not silently end it. The
	// growth is capped (a FAT name is <= 255 UTF-16 units ≈ 600 bytes; a
	// firmware size beyond the cap is bogus, not a name) so a bad required-
	// size cannot force a huge allocation.
	constexpr USIZE MAX_UEFI_READ_BUFFER = 64 * 1024;
	UINT8 stackBuffer[512];
	UINT8 *buffer = stackBuffer;
	USIZE bufferSize = sizeof(stackBuffer);

	EFI_STATUS status;
	while (true)
	{
		status = fp->Read(fp, &bufferSize, buffer);

		if (!EFI_ERROR_CHECK(status))
			break;
		if (status != (EFI_STATUS)EFI_BUFFER_TOO_SMALL || buffer != stackBuffer)
			break;

		// Read reported the required size in bufferSize — grow (capped) and retry.
		if (bufferSize > MAX_UEFI_READ_BUFFER)
			break; // bogus required size — surface the original status
		UINT8 *grown = new UINT8[bufferSize];
		if (grown == nullptr)
			break;
		buffer = grown;
	}

	// The heap buffer (when taken) is released at the end of this iteration.
	struct HeapGuard
	{
		UINT8 *ptr;
		~HeapGuard() { delete[] ptr; }
	} heapGuard{buffer == stackBuffer ? nullptr : buffer};

	if (EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_ReadFailed);

	// End of directory — the CLEAN-end sentinel, not a failure.
	if (bufferSize == 0)
		return Result<VOID, Error>::Err(Error::Fs_NoMoreEntries);

	EFI_FILE_INFO *fileInfo = (EFI_FILE_INFO *)buffer;

	// Copy filename to currentEntry
	INT32 i = 0;
	while (fileInfo->FileName[i] != 0 && i < 255)
	{
		currentEntry.Name[i] = fileInfo->FileName[i];
		i++;
	}
	currentEntry.Name[i] = 0;

	// Fill other fields
	currentEntry.Size = fileInfo->FileSize;
	currentEntry.IsDirectory = (fileInfo->Attribute & EFI_FILE_DIRECTORY) != 0;
	currentEntry.IsDrive = false;
	currentEntry.VolumeSerial = 0;
	currentEntry.IsHidden = (fileInfo->Attribute & EFI_FILE_HIDDEN) != 0;
	currentEntry.IsSystem = (fileInfo->Attribute & EFI_FILE_SYSTEM) != 0;
	currentEntry.IsReadOnly = (fileInfo->Attribute & EFI_FILE_READ_ONLY) != 0;
	currentEntry.Type = 0;
	currentEntry.CreationTime = 0;
	currentEntry.LastModifiedTime = 0;

	return Result<VOID, Error>::Ok();
}

BOOL DirectoryIterator::IsValid() const
{
	return handle != nullptr;
}
