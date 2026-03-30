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

	// Allocate buffer for EFI_FILE_INFO (needs to include variable-length filename)
	// Use a fixed buffer size that should be large enough for most filenames
	UINT8 buffer[512];
	USIZE bufferSize = sizeof(buffer);

	EFI_STATUS status = fp->Read(fp, &bufferSize, buffer);

	if (EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_ReadFailed);

	// End of directory
	if (bufferSize == 0)
		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

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
