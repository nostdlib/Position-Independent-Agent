/**
 * @file pipe.cc
 * @brief Windows anonymous pipe implementation
 *
 * @details Pipe creation via Kernel32 CreatePipe. Read/write via ZwReadFile
 * and ZwWriteFile. Handle cleanup via ZwClose.
 */

#include "platform/system/pipe.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/kernel/windows/kernel32.h"
#include "core/memory/memory.h"

// ============================================================================
// Pipe::Create
// ============================================================================

Result<Pipe, Error> Pipe::Create() noexcept
{
	PVOID readHandle = nullptr;
	PVOID writeHandle = nullptr;

	auto bufferSize = (32 * 1024 * 1024);

	auto result = Kernel32::CreatePipe(&readHandle, &writeHandle, nullptr, 0);
	if (result.IsErr())
	{
		return Result<Pipe, Error>::Err(result, Error::Pipe_CreateFailed);
	}

	result = Kernel32::SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, bufferSize);

	if (result.IsErr())
	{
		(VOID)NTDLL::ZwClose(readHandle);
		(VOID)NTDLL::ZwClose(writeHandle);
		return Result<Pipe, Error>::Err(result, Error::Pipe_CreateFailed);
	}

	return Result<Pipe, Error>::Ok(Pipe((SSIZE)readHandle, (SSIZE)writeHandle));
}

// ============================================================================
// Pipe::Read
// ============================================================================

Result<USIZE, Error> Pipe::Read(Span<UINT8> buffer) noexcept
{
	if (readFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pipe_ReadFailed);

	UINT32 bytesAvailable = 0;

	// Check if there is data without "consuming" it
	if (Kernel32::PeekNamedPipe(readFd, nullptr, 0, nullptr, &bytesAvailable, nullptr))
	{
		if (bytesAvailable == 0)
		{
			return Result<USIZE, Error>::Ok(0);
		}
	}

	IO_STATUS_BLOCK iosb;
	Memory::Zero(&iosb, sizeof(IO_STATUS_BLOCK));
	auto result = NTDLL::ZwReadFile(
		(PVOID)(USIZE)readFd, nullptr, nullptr, nullptr,
		&iosb, buffer.Data(), (UINT32)buffer.Size(), nullptr, nullptr);

	if (result.IsErr())
	{
		return Result<USIZE, Error>::Err(result, Error::Pipe_ReadFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)iosb.Information);
}

// ============================================================================
// Pipe::Write
// ============================================================================

Result<USIZE, Error> Pipe::Write(Span<const UINT8> data) noexcept
{
	if (writeFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pipe_ReadFailed);

	IO_STATUS_BLOCK iosb;
	Memory::Zero(&iosb, sizeof(IO_STATUS_BLOCK));
	auto result = NTDLL::ZwWriteFile(
		(PVOID)(USIZE)writeFd, nullptr, nullptr, nullptr,
		&iosb, (PVOID)data.Data(), (UINT32)data.Size(), nullptr, nullptr);

	if (result.IsErr())
	{
		return Result<USIZE, Error>::Err(result, Error::Pipe_ReadFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)iosb.Information);
}

// ============================================================================
// Pipe::CloseRead
// ============================================================================

Result<VOID, Error> Pipe::CloseRead() noexcept
{
	if (readFd == INVALID_FD)
		return Result<VOID, Error>::Ok();

	(VOID)NTDLL::ZwClose((PVOID)(USIZE)readFd);
	readFd = INVALID_FD;
	return Result<VOID, Error>::Ok();
}

// ============================================================================
// Pipe::CloseWrite
// ============================================================================

Result<VOID, Error> Pipe::CloseWrite() noexcept
{
	if (writeFd == INVALID_FD)
		return Result<VOID, Error>::Ok();

	(VOID)NTDLL::ZwClose((PVOID)(USIZE)writeFd);
	writeFd = INVALID_FD;
	return Result<VOID, Error>::Ok();
}

// ============================================================================
// Pipe::Close
// ============================================================================

Result<VOID, Error> Pipe::Close() noexcept
{
	(VOID)CloseRead();
	(VOID)CloseWrite();
	return Result<VOID, Error>::Ok();
}
