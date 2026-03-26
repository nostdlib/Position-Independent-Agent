/**
 * @file pipe.cc
 * @brief UEFI pipe stub
 *
 * @details UEFI does not support pipes. All operations return failure.
 */

#include "platform/system/pipe.h"

Result<Pipe, Error> Pipe::Create() noexcept
{
	return Result<Pipe, Error>::Err(Error::Pipe_NotSupported);
}

Result<USIZE, Error> Pipe::Read([[maybe_unused]] Span<UINT8> buffer) noexcept
{
	return Result<USIZE, Error>::Err(Error::Pipe_NotSupported);
}

Result<USIZE, Error> Pipe::Write([[maybe_unused]] Span<const UINT8> data) noexcept
{
	return Result<USIZE, Error>::Err(Error::Pipe_NotSupported);
}

Result<void, Error> Pipe::CloseRead() noexcept
{
	readFd = INVALID_FD;
	return Result<void, Error>::Ok();
}

Result<void, Error> Pipe::CloseWrite() noexcept
{
	writeFd = INVALID_FD;
	return Result<void, Error>::Ok();
}

Result<void, Error> Pipe::Close() noexcept
{
	readFd = INVALID_FD;
	writeFd = INVALID_FD;
	return Result<void, Error>::Ok();
}
