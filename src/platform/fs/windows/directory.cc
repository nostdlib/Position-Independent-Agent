#include "platform/fs/directory.h"
#include "platform/console/logger.h"
#include "core/types/primitives.h"
#include "core/memory/memory.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/kernel/windows/ntdll.h"

Result<VOID, Error> Directory::Create(PCWCHAR path)
{
	PVOID hDir;
	UNICODE_STRING uniName;
	OBJECT_ATTRIBUTES objAttr;
	IO_STATUS_BLOCK ioStatusBlock;

	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &uniName, nullptr, nullptr);
	if (!pathResult)
		return Result<VOID, Error>::Err(pathResult, Error::Fs_CreateDirFailed);

	InitializeObjectAttributes(&objAttr, &uniName, 0, nullptr, nullptr);

	auto createResult = NTDLL::ZwCreateFile(
		&hDir,
		FILE_LIST_DIRECTORY | SYNCHRONIZE,
		&objAttr,
		&ioStatusBlock,
		nullptr,
		FILE_ATTRIBUTE_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_OPEN_IF,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
		nullptr,
		0);

	NTDLL::RtlFreeUnicodeString(&uniName);

	if (createResult)
	{
		(VOID)NTDLL::ZwClose(hDir);
		return Result<VOID, Error>::Ok();
	}
	LOG_ERROR("Directory::Create failed: errors=%e path=%ls", createResult.Error(), path);
	return Result<VOID, Error>::Err(createResult, Error::Fs_CreateDirFailed);
}

Result<VOID, Error> Directory::Delete(PCWCHAR path)
{
	PVOID hDir;
	FILE_DISPOSITION_INFORMATION disp;
	UNICODE_STRING uniName;
	OBJECT_ATTRIBUTES objAttr;
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&disp, sizeof(FILE_DISPOSITION_INFORMATION));
	disp.DeleteFile = true;

	auto pathResult = NTDLL::RtlDosPathNameToNtPathName_U(path, &uniName, nullptr, nullptr);
	if (!pathResult)
		return Result<VOID, Error>::Err(pathResult, Error::Fs_DeleteDirFailed);

	InitializeObjectAttributes(&objAttr, &uniName, 0, nullptr, nullptr);

	auto openResult = NTDLL::ZwOpenFile(&hDir, DELETE | SYNCHRONIZE, &objAttr, &ioStatusBlock, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (!openResult)
	{
		NTDLL::RtlFreeUnicodeString(&uniName);
		return Result<VOID, Error>::Err(openResult, Error::Fs_DeleteDirFailed);
	}

	auto setResult = NTDLL::ZwSetInformationFile(
		hDir,
		&ioStatusBlock,
		&disp,
		sizeof(disp),
		FileDispositionInformation);

	(VOID)NTDLL::ZwClose(hDir);
	NTDLL::RtlFreeUnicodeString(&uniName);

	if (!setResult)
		return Result<VOID, Error>::Err(setResult, Error::Fs_DeleteDirFailed);

	return Result<VOID, Error>::Ok();
}
