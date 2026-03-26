/**
 * @file shell_process.cc
 * @brief UEFI ShellProcess stub
 *
 * @details UEFI does not support interactive shell processes.
 * Create() returns failure; all other operations are unreachable.
 */

#include "platform/system/shell_process.h"

CHAR ShellProcess::EndOfLineChar() noexcept
{
	return '>';
}

Result<ShellProcess, Error> ShellProcess::Create() noexcept
{
	return Result<ShellProcess, Error>::Err(Error::ShellProcess_NotSupported);
}

Result<USIZE, Error> ShellProcess::Write([[maybe_unused]] const CHAR *data, [[maybe_unused]] USIZE length) noexcept
{
	return Result<USIZE, Error>::Err(Error::ShellProcess_NotSupported);
}

Result<USIZE, Error> ShellProcess::Read([[maybe_unused]] CHAR *buffer, [[maybe_unused]] USIZE capacity) noexcept
{
	return Result<USIZE, Error>::Err(Error::ShellProcess_NotSupported);
}

Result<USIZE, Error> ShellProcess::ReadError([[maybe_unused]] CHAR *buffer, [[maybe_unused]] USIZE capacity) noexcept
{
	return Result<USIZE, Error>::Err(Error::ShellProcess_NotSupported);
}

SSIZE ShellProcess::Poll([[maybe_unused]] SSIZE timeoutMs) noexcept
{
	return -1;
}
