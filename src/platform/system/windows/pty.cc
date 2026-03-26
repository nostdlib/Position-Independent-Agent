/**
 * @file pty.cc
 * @brief Windows PTY stub
 *
 * @details Windows does not support POSIX PTYs. All operations return failure.
 * Shell processes on Windows use Pipe-based I/O instead (see ShellProcess).
 */

#include "platform/system/pty.h"

Result<Pty, Error> Pty::Create() noexcept
{
	return Result<Pty, Error>::Err(Error::Pty_NotSupported);
}

Result<USIZE, Error> Pty::Read([[maybe_unused]] Span<UINT8> buffer) noexcept
{
	return Result<USIZE, Error>::Err(Error::Pty_NotSupported);
}

Result<USIZE, Error> Pty::Write([[maybe_unused]] Span<const UINT8> data) noexcept
{
	return Result<USIZE, Error>::Err(Error::Pty_NotSupported);
}

SSIZE Pty::Poll([[maybe_unused]] SSIZE timeoutMs) noexcept
{
	return -1;
}

Result<void, Error> Pty::CloseSlave() noexcept
{
	slaveFd = INVALID_FD;
	return Result<void, Error>::Ok();
}

Result<void, Error> Pty::Close() noexcept
{
	masterFd = INVALID_FD;
	slaveFd = INVALID_FD;
	return Result<void, Error>::Ok();
}
