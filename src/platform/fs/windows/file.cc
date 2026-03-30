#include "platform/fs/file.h"
#include "platform/console/logger.h"
#include "core/types/primitives.h"
#include "core/string/string.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/kernel/windows/ntdll.h"

// --- Internal Constructor (trivial — never fails) ---
File::File(PVOID handle, USIZE size) : fileHandle(handle), fileSize(size) {}

// --- Factory & Static Operations ---
Result<File, Error> File::Open(PCWCHAR path, INT32 flags)
{
	UINT32 dwDesiredAccess = 0;
	UINT32 dwShareMode = FILE_SHARE_READ;
	UINT32 dwCreationDisposition = FILE_OPEN;
	UINT32 ntFlags = 0;
	UINT32 fileAttributes = FILE_ATTRIBUTE_NORMAL;

	// 1. Map Access Flags
	if (flags & File::ModeRead)
		dwDesiredAccess |= GENERIC_READ;
	if (flags & File::ModeWrite)
		dwDesiredAccess |= GENERIC_WRITE;
	if (flags & File::ModeAppend)
		dwDesiredAccess |= FILE_APPEND_DATA;

	// 2. Map Creation/Truncation Flags
	if (flags & File::ModeCreate)
	{
		if (flags & File::ModeTruncate)
			dwCreationDisposition = FILE_OVERWRITE_IF;
		else
			dwCreationDisposition = FILE_OPEN_IF;
	}
	else if (flags & File::ModeTruncate)
	{
		dwCreationDisposition = FILE_OVERWRITE;
	}

	// Synchronous I/O — PIR never uses overlapped file handles
	ntFlags |= FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE;

	// Always allow waiting and querying attributes
	dwDesiredAccess |= SYNCHRONIZE | FILE_READ_ATTRIBUTES;

	// Convert DOS path to NT path
	UNICODE_STRING ntPathU;
	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &ntPathU, nullptr, nullptr);
	if (!pathResult)
		return Result<File, Error>::Err(pathResult, Error::Fs_OpenFailed);

	OBJECT_ATTRIBUTES objAttr;
	InitializeObjectAttributes(&objAttr, &ntPathU, 0, nullptr, nullptr);

	IO_STATUS_BLOCK ioStatusBlock;
	PVOID hFile = nullptr;

	auto createResult = NTDLL::ZwCreateFile(
		&hFile,
		dwDesiredAccess,
		&objAttr,
		&ioStatusBlock,
		nullptr,
		fileAttributes,
		dwShareMode,
		dwCreationDisposition,
		ntFlags,
		nullptr,
		0);

	NTDLL::RtlFreeUnicodeString(&ntPathU);

	if (!createResult || hFile == INVALID_HANDLE_VALUE)
		return Result<File, Error>::Err(createResult, Error::Fs_OpenFailed);

	// Query file size before constructing the File (keeps the constructor trivial)
	USIZE size = 0;
	FILE_STANDARD_INFORMATION fileStandardInfo;
	IO_STATUS_BLOCK sizeIoBlock;
	Memory::Zero(&fileStandardInfo, sizeof(FILE_STANDARD_INFORMATION));
	Memory::Zero(&sizeIoBlock, sizeof(IO_STATUS_BLOCK));
	auto sizeResult = NTDLL::ZwQueryInformationFile(hFile, &sizeIoBlock, &fileStandardInfo, sizeof(fileStandardInfo), FileStandardInformation);
	if (sizeResult)
		size = fileStandardInfo.EndOfFile.QuadPart;

	return Result<File, Error>::Ok(File((PVOID)hFile, size));
}

Result<VOID, Error> File::Delete(PCWCHAR path)
{
	UNICODE_STRING ntName;
	OBJECT_ATTRIBUTES attr;
	PVOID hFile = nullptr;
	IO_STATUS_BLOCK io;

	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &ntName, nullptr, nullptr);
	if (!pathResult)
		return Result<VOID, Error>::Err(pathResult, Error::Fs_DeleteFailed);

	InitializeObjectAttributes(&attr, &ntName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	auto createResult = NTDLL::ZwCreateFile(&hFile, SYNCHRONIZE | DELETE, &attr, &io, nullptr, 0,
											FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
											FILE_OPEN, FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE, nullptr, 0);

	if (!createResult)
	{
		NTDLL::RtlFreeUnicodeString(&ntName);
		return Result<VOID, Error>::Err(createResult, Error::Fs_DeleteFailed);
	}

	(VOID)NTDLL::ZwClose(hFile);
	NTDLL::RtlFreeUnicodeString(&ntName);
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> File::Exists(PCWCHAR path)
{
	OBJECT_ATTRIBUTES objAttr;
	UNICODE_STRING uniName;
	FILE_BASIC_INFORMATION fileBasicInfo;

	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &uniName, nullptr, nullptr);
	if (!pathResult)
		return Result<VOID, Error>::Err(pathResult, Error::Fs_OpenFailed);

	InitializeObjectAttributes(&objAttr, &uniName, 0, nullptr, nullptr);
	auto queryResult = NTDLL::ZwQueryAttributesFile(&objAttr, &fileBasicInfo);

	NTDLL::RtlFreeUnicodeString(&uniName);

	if (!queryResult)
		return Result<VOID, Error>::Err(queryResult, Error::Fs_OpenFailed);

	if (fileBasicInfo.FileAttributes == 0xFFFFFFFF)
		return Result<VOID, Error>::Err(Error::Fs_OpenFailed);

	return Result<VOID, Error>::Ok();
}

// --- Move Semantics ---
File::File(File &&other) noexcept : fileHandle(nullptr), fileSize(0)
{
	fileHandle = other.fileHandle;
	fileSize = other.fileSize;
	other.fileHandle = nullptr;
	other.fileSize = 0;
}

// Operator move assignment
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

// --- Logic ---
BOOL File::IsValid() const
{
	// Windows returns INVALID_HANDLE_VALUE (-1) on many errors,
	// but some APIs return nullptr. We check for both.
	return fileHandle != nullptr && fileHandle != INVALID_HANDLE_VALUE;
}

// Close the file handle
VOID File::Close()
{
	if (IsValid())
	{
		(VOID)NTDLL::ZwClose((PVOID)fileHandle);
		fileHandle = nullptr;
		fileSize = 0;
	}
}

// Read data from the file into the buffer
Result<UINT32, Error> File::Read(Span<UINT8> buffer)
{
	if (!IsValid())
		return Result<UINT32, Error>::Err(Error::Fs_ReadFailed);

	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	auto readResult = NTDLL::ZwReadFile((PVOID)fileHandle, nullptr, nullptr, nullptr, &ioStatusBlock, buffer.Data(), (UINT32)buffer.Size(), nullptr, nullptr);

	if (readResult)
	{
		return Result<UINT32, Error>::Ok((UINT32)ioStatusBlock.Information);
	}
	return Result<UINT32, Error>::Err(readResult, Error::Fs_ReadFailed);
}

// Write data from the buffer to the file
Result<UINT32, Error> File::Write(Span<const UINT8> buffer)
{
	if (!IsValid())
		return Result<UINT32, Error>::Err(Error::Fs_WriteFailed);

	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	auto writeResult = NTDLL::ZwWriteFile((PVOID)fileHandle, nullptr, nullptr, nullptr, &ioStatusBlock, (PVOID)buffer.Data(), (UINT32)buffer.Size(), nullptr, nullptr);

	if (writeResult)
	{
		return Result<UINT32, Error>::Ok((UINT32)ioStatusBlock.Information);
	}
	return Result<UINT32, Error>::Err(writeResult, Error::Fs_WriteFailed);
}

Result<USIZE, Error> File::GetOffset() const
{
	if (!IsValid())
		return Result<USIZE, Error>::Err(Error::Fs_SeekFailed);

	FILE_POSITION_INFORMATION posFile;
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&posFile, sizeof(posFile));
	Memory::Zero(&ioStatusBlock, sizeof(ioStatusBlock));

	auto queryResult = NTDLL::ZwQueryInformationFile((PVOID)fileHandle, &ioStatusBlock, &posFile, sizeof(posFile), FilePositionInformation);
	if (queryResult)
		return Result<USIZE, Error>::Ok((USIZE)posFile.CurrentByteOffset.QuadPart);
	return Result<USIZE, Error>::Err(queryResult, Error::Fs_SeekFailed);
}

Result<VOID, Error> File::SetOffset(USIZE absoluteOffset)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	FILE_POSITION_INFORMATION posInfo;
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&posInfo, sizeof(FILE_POSITION_INFORMATION));
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));
	posInfo.CurrentByteOffset.QuadPart = (INT64)absoluteOffset;

	auto setResult = NTDLL::ZwSetInformationFile((PVOID)fileHandle, &ioStatusBlock, &posInfo, sizeof(posInfo), FilePositionInformation);
	if (setResult)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(setResult, Error::Fs_SeekFailed);
}

Result<VOID, Error> File::MoveOffset(SSIZE relativeAmount, OffsetOrigin origin)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	IO_STATUS_BLOCK ioStatusBlock;
	FILE_POSITION_INFORMATION posInfo;
	FILE_STANDARD_INFORMATION fileStandardInfo;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));
	Memory::Zero(&posInfo, sizeof(FILE_POSITION_INFORMATION));
	Memory::Zero(&fileStandardInfo, sizeof(FILE_STANDARD_INFORMATION));
	INT64 distance = 0;

	auto queryResult = NTDLL::ZwQueryInformationFile((PVOID)fileHandle, &ioStatusBlock, &posInfo, sizeof(posInfo), FilePositionInformation);
	if (!queryResult)
		return Result<VOID, Error>::Err(queryResult, Error::Fs_SeekFailed);

	switch (origin)
	{
	case OffsetOrigin::Start:
		distance = relativeAmount;
		break;
	case OffsetOrigin::Current:
		distance = posInfo.CurrentByteOffset.QuadPart + relativeAmount;
		break;
	case OffsetOrigin::End:
		queryResult = NTDLL::ZwQueryInformationFile((PVOID)fileHandle, &ioStatusBlock, &fileStandardInfo, sizeof(fileStandardInfo), FileStandardInformation);
		if (!queryResult)
			return Result<VOID, Error>::Err(queryResult, Error::Fs_SeekFailed);
		distance = fileStandardInfo.EndOfFile.QuadPart + relativeAmount;
		break;
	default:
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);
	}
	posInfo.CurrentByteOffset.QuadPart = distance;

	auto setResult = NTDLL::ZwSetInformationFile((PVOID)fileHandle, &ioStatusBlock, &posInfo, sizeof(posInfo), FilePositionInformation);
	if (setResult)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(setResult, Error::Fs_SeekFailed);
}
