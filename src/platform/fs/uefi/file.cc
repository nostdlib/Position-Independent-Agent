/**
 * file.cc - UEFI File Implementation
 *
 * Implements File class operations using EFI_FILE_PROTOCOL.
 */

#include "platform/fs/file.h"
#include "platform/fs/uefi/uefi_fs_helpers.h"
#include "core/memory/memory.h"

// EFI_FILE_INFO_ID {09576E92-6D3F-11D2-8E39-00A0C969723B}
static NOINLINE EFI_GUID MakeFileInfoGuid()
{
	EFI_GUID g;
	g.Data1 = 0x09576E92;
	g.Data2 = 0x6D3F;
	g.Data3 = 0x11D2;
	g.Data4[0] = 0x8E; g.Data4[1] = 0x39; g.Data4[2] = 0x00; g.Data4[3] = 0xA0;
	g.Data4[4] = 0xC9; g.Data4[5] = 0x69; g.Data4[6] = 0x72; g.Data4[7] = 0x3B;
	return g;
}

// =============================================================================
// Helper: Query file size via EFI_FILE_INFO
// =============================================================================

static NOINLINE USIZE QueryFileSize(EFI_FILE_PROTOCOL &fp)
{
	EFI_GUID fileInfoId = MakeFileInfoGuid();
	USIZE infoSize = 0;
	fp.GetInfo(&fp, &fileInfoId, &infoSize, nullptr);
	if (infoSize == 0)
		return 0;

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	USIZE size = 0;
	EFI_FILE_INFO *fileInfo = nullptr;
	if (!EFI_ERROR_CHECK(bs->AllocatePool(EfiLoaderData, infoSize, (PVOID *)&fileInfo)))
	{
		if (!EFI_ERROR_CHECK(fp.GetInfo(&fp, &fileInfoId, &infoSize, fileInfo)))
			size = fileInfo->FileSize;
		bs->FreePool(fileInfo);
	}
	return size;
}

// =============================================================================
// Helper: Truncate file to zero length via EFI_FILE_INFO
// =============================================================================

static NOINLINE BOOL TruncateFile(EFI_FILE_PROTOCOL &fp)
{
	EFI_GUID fileInfoId = MakeFileInfoGuid();
	USIZE infoSize = 0;
	fp.GetInfo(&fp, &fileInfoId, &infoSize, nullptr);
	if (infoSize == 0)
		return false;

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	BOOL success = false;
	EFI_FILE_INFO *fileInfo = nullptr;
	if (!EFI_ERROR_CHECK(bs->AllocatePool(EfiLoaderData, infoSize, (PVOID *)&fileInfo)))
	{
		if (!EFI_ERROR_CHECK(fp.GetInfo(&fp, &fileInfoId, &infoSize, fileInfo)))
		{
			fileInfo->FileSize = 0;
			success = !EFI_ERROR_CHECK(fp.SetInfo(&fp, &fileInfoId, infoSize, fileInfo));
		}
		bs->FreePool(fileInfo);
	}
	return success;
}

// =============================================================================
// File Class Implementation
// =============================================================================

// --- Internal Constructor (trivial — never fails) ---
File::File(PVOID handle, USIZE size) : fileHandle(handle), fileSize(size) {}

// --- Factory & Static Operations ---
Result<File, Error> File::Open(PCWCHAR path, INT32 flags)
{
	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<File, Error>::Err(Error::Fs_OpenFailed);

	// Convert flags to EFI modes
	UINT64 mode = 0;
	UINT64 attributes = 0;

	if (flags & File::ModeRead)
		mode |= EFI_FILE_MODE_READ;
	if (flags & File::ModeWrite)
		mode |= EFI_FILE_MODE_WRITE;
	if (flags & File::ModeCreate)
		mode |= EFI_FILE_MODE_CREATE;

	// If no mode specified, default to read
	if (mode == 0)
		mode = EFI_FILE_MODE_READ;

	// Create mode requires write mode
	if (mode & EFI_FILE_MODE_CREATE)
		mode |= EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE;

	EFI_FILE_PROTOCOL *handle = OpenFileFromRoot(root, path, mode, attributes);
	root->Close(root);

	if (handle == nullptr)
		return Result<File, Error>::Err(Error::Fs_OpenFailed);

	// Handle truncate flag
	if (flags & File::ModeTruncate)
	{
		if (!TruncateFile(*handle))
		{
			handle->Close(handle);
			return Result<File, Error>::Err(Error::Fs_OpenFailed);
		}
	}

	// Query file size before constructing the File (keeps the constructor trivial)
	USIZE size = QueryFileSize(*handle);

	return Result<File, Error>::Ok(File((PVOID)handle, size));
}

Result<VOID, Error> File::Delete(PCWCHAR path)
{
	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_DeleteFailed);

	EFI_FILE_PROTOCOL *handle = OpenFileFromRoot(root, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
	root->Close(root);

	if (handle == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_DeleteFailed);

	// EFI_FILE_PROTOCOL.Delete closes the handle and deletes the file
	EFI_STATUS status = handle->Delete(handle);
	if (EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_DeleteFailed);
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> File::Exists(PCWCHAR path)
{
	EFI_FILE_PROTOCOL *root = GetRootDirectory();
	if (root == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_OpenFailed);

	EFI_FILE_PROTOCOL *handle = OpenFileFromRoot(root, path, EFI_FILE_MODE_READ, 0);
	root->Close(root);

	if (handle == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_OpenFailed);

	handle->Close(handle);
	return Result<VOID, Error>::Ok();
}

BOOL File::IsValid() const
{
	return fileHandle != nullptr;
}

VOID File::Close()
{
	if (IsValid())
	{
		EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
		fp->Close(fp);
		fileHandle = nullptr;
	}
	fileSize = 0;
}

Result<UINT32, Error> File::Read(Span<UINT8> buffer)
{
	if (!IsValid() || buffer.Data() == nullptr || buffer.Size() == 0)
		return Result<UINT32, Error>::Err(Error::Fs_ReadFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
	USIZE readSize = buffer.Size();

	EFI_STATUS status = fp->Read(fp, &readSize, buffer.Data());
	if (EFI_ERROR_CHECK(status))
		return Result<UINT32, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_ReadFailed);

	return Result<UINT32, Error>::Ok((UINT32)readSize);
}

Result<UINT32, Error> File::Write(Span<const UINT8> buffer)
{
	if (!IsValid() || buffer.Data() == nullptr || buffer.Size() == 0)
		return Result<UINT32, Error>::Err(Error::Fs_WriteFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
	USIZE writeSize = buffer.Size();

	EFI_STATUS status = fp->Write(fp, &writeSize, (PVOID)buffer.Data());
	if (EFI_ERROR_CHECK(status))
		return Result<UINT32, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_WriteFailed);

	// Update file size if we wrote past the end
	UINT64 pos = 0;
	if (!EFI_ERROR_CHECK(fp->GetPosition(fp, &pos)))
	{
		if (pos > fileSize)
			fileSize = pos;
	}

	return Result<UINT32, Error>::Ok((UINT32)writeSize);
}

Result<USIZE, Error> File::GetOffset() const
{
	if (!IsValid())
		return Result<USIZE, Error>::Err(Error::Fs_SeekFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
	UINT64 position = 0;
	EFI_STATUS status = fp->GetPosition(fp, &position);
	if (!EFI_ERROR_CHECK(status))
		return Result<USIZE, Error>::Ok((USIZE)position);
	return Result<USIZE, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::SetOffset(USIZE absoluteOffset)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
	EFI_STATUS status = fp->SetPosition(fp, absoluteOffset);
	if (!EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::MoveOffset(SSIZE relativeAmount, OffsetOrigin origin)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)fileHandle;
	UINT64 newPosition = 0;

	switch (origin)
	{
	case OffsetOrigin::Start:
		newPosition = (relativeAmount >= 0) ? (UINT64)relativeAmount : 0;
		break;
	case OffsetOrigin::Current:
	{
		UINT64 currentPos = 0;
		EFI_STATUS getStatus = fp->GetPosition(fp, &currentPos);
		if (EFI_ERROR_CHECK(getStatus))
			return Result<VOID, Error>::Err(Error::Uefi((UINT32)getStatus), Error::Fs_SeekFailed);
		if (relativeAmount >= 0)
			newPosition = currentPos + relativeAmount;
		else
			newPosition = (currentPos > (UINT64)(-relativeAmount)) ? currentPos + relativeAmount : 0;
	}
	break;
	case OffsetOrigin::End:
		if (relativeAmount >= 0)
			newPosition = fileSize + relativeAmount;
		else
			newPosition = (fileSize > (UINT64)(-relativeAmount)) ? fileSize + relativeAmount : 0;
		break;
	default:
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);
	}

	EFI_STATUS status = fp->SetPosition(fp, newPosition);
	if (!EFI_ERROR_CHECK(status))
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Uefi((UINT32)status), Error::Fs_SeekFailed);
}

File::File(File &&other) noexcept
	: fileHandle(other.fileHandle), fileSize(other.fileSize)
{
	other.fileHandle = nullptr;
	other.fileSize = 0;
}

File &File::operator=(File &&other) noexcept
{
	if (this != &other)
	{
		if (IsValid())
			(VOID)Close();
		fileHandle = other.fileHandle;
		fileSize = other.fileSize;
		other.fileHandle = nullptr;
		other.fileSize = 0;
	}
	return *this;
}
