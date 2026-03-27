/**
 * directory.cc - UEFI Directory Implementation
 *
 * Implements Directory static methods using EFI_FILE_PROTOCOL.
 */

#include "platform/fs/directory.h"
#include "platform/fs/uefi/uefi_fs_helpers.h"
#include "platform/fs/path.h"
#include "core/memory/memory.h"

// =============================================================================
// Directory Implementation
// =============================================================================

Result<VOID, Error> Directory::Create(PCWCHAR path)
{
	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_CreateDirFailed);

	// Normalize path separators (convert '/' to '\' for UEFI)
	WCHAR normalizedBuf[512];
	if (!Path::NormalizePath(path, Span<WCHAR>(normalizedBuf)))
	{
		root->Close(root);
		return Result<VOID, Error>::Err(Error::Fs_PathResolveFailed, Error::Fs_CreateDirFailed);
	}

	EFI_FILE_PROTOCOL *dirHandle = nullptr;
	EFI_STATUS status = root->Open(root, &dirHandle, (CHAR16 *)normalizedBuf,
								   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
								   EFI_FILE_DIRECTORY);
	root->Close(root);

	if (EFI_ERROR_CHECK(status) || dirHandle == nullptr)
		return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_CreateDirFailed);

	dirHandle->Close(dirHandle);
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Directory::Delete(PCWCHAR path)
{
	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_DeleteDirFailed);

	EFI_FILE_PROTOCOL *dirHandle = OpenFileFromRoot(root, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
	root->Close(root);

	if (dirHandle == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_DeleteDirFailed);

	// EFI_FILE_PROTOCOL.Delete works for both files and directories
	EFI_STATUS status = dirHandle->Delete(dirHandle);
	if (EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_DeleteDirFailed);
	return Result<VOID, Error>::Ok();
}
