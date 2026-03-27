/**
 * @file pty.cc
 * @brief UEFI PTY stub
 *
 * @details UEFI does not support PTYs. All operations return failure.
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

Result<VOID, Error> Pty::CloseSlave() noexcept
{
	slaveFd = INVALID_FD;
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Pty::Close() noexcept
{
	masterFd = INVALID_FD;
	slaveFd = INVALID_FD;
	return Result<VOID, Error>::Ok();
}
