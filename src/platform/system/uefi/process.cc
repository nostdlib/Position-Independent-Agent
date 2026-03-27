/**
 * @file process.cc
 * @brief UEFI process management stub
 *
 * @details UEFI does not support process creation. All operations return failure.
 */

#include "platform/system/process.h"

Result<Process, Error> Process::Create(
	[[maybe_unused]] const CHAR *path,
	[[maybe_unused]] const CHAR *const args[],
	[[maybe_unused]] SSIZE stdinFd,
	[[maybe_unused]] SSIZE stdoutFd,
	[[maybe_unused]] SSIZE stderrFd) noexcept
{
	return Result<Process, Error>::Err(Error::Process_NotSupported);
}

Result<SSIZE, Error> Process::Wait() noexcept
{
	return Result<SSIZE, Error>::Err(Error::Process_NotSupported);
}

Result<VOID, Error> Process::Terminate() noexcept
{
	return Result<VOID, Error>::Err(Error::Process_NotSupported);
}

BOOL Process::IsRunning() const noexcept
{
	return false;
}

Result<VOID, Error> Process::Close() noexcept
{
	id = INVALID_ID;
	return Result<VOID, Error>::Ok();
}
