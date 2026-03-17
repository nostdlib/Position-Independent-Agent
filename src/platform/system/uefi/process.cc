/**
 * @file process.cc
 * @brief UEFI process management stub
 *
 * @details UEFI does not support process creation. All operations return failure.
 */

#include "platform/system/process.h"

Result<Process, Error> Process::Create(
	const CHAR *path,
	const CHAR *const args[],
	SSIZE stdinFd,
	SSIZE stdoutFd,
	SSIZE stderrFd) noexcept
{
	(void)path;
	(void)args;
	(void)stdinFd;
	(void)stdoutFd;
	(void)stderrFd;
	return Result<Process, Error>::Err(Error::Process_NotSupported);
}

Result<SSIZE, Error> Process::Wait() noexcept
{
	return Result<SSIZE, Error>::Err(Error::Process_NotSupported);
}

Result<void, Error> Process::Terminate() noexcept
{
	return Result<void, Error>::Err(Error::Process_NotSupported);
}

BOOL Process::IsRunning() const noexcept
{
	return false;
}

Result<void, Error> Process::Close() noexcept
{
	id = INVALID_ID;
	return Result<void, Error>::Ok();
}
