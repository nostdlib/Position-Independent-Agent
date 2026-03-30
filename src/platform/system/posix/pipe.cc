/**
 * @file pipe.cc
 * @brief Shared POSIX anonymous pipe implementation
 *
 * @details Cross-platform pipe creation via pipe()/pipe2() syscall with
 * read/write/close via standard POSIX syscalls. Platform differences:
 * - Linux aarch64/riscv: pipe2() instead of pipe()
 * - Solaris: pipe() returns both fds via syscall convention
 * - macOS/FreeBSD: standard pipe() syscall
 */

#include "platform/system/pipe.h"
#if defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#elif defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#elif defined(PLATFORM_MACOS)
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#elif defined(PLATFORM_IOS)
#include "platform/kernel/ios/syscall.h"
#include "platform/kernel/ios/system.h"
#elif defined(PLATFORM_SOLARIS)
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"
#elif defined(PLATFORM_FREEBSD)
#include "platform/kernel/freebsd/syscall.h"
#include "platform/kernel/freebsd/system.h"
#endif

// ============================================================================
// Pipe::Create
// ============================================================================

Result<Pipe, Error> Pipe::Create() noexcept
{
	INT32 fds[2]{};

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))
	// aarch64/riscv: use pipe2 with flags=0
	SSIZE result = System::Call(SYS_PIPE2, (USIZE)fds, 0);
#else
	SSIZE result = System::Call(SYS_PIPE, (USIZE)fds);
#endif
	// Validate the result of syscall
	if (result < 0)
	{
		return Result<Pipe, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Pipe_CreateFailed);
	}
	
	return Result<Pipe, Error>::Ok(Pipe((SSIZE)fds[0], (SSIZE)fds[1]));
}

// ============================================================================
// Pipe::Read
// ============================================================================

Result<USIZE, Error> Pipe::Read(Span<UINT8> buffer) noexcept
{
	// Validate the read end of the pipe
	if (readFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pipe_ReadFailed);
	// System call to read from the pipe
	SSIZE result = System::Call(SYS_READ, (USIZE)readFd, (USIZE)buffer.Data(), buffer.Size());

	if (result < 0)
	{
		return Result<USIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Pipe_ReadFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)result);
}

// ============================================================================
// Pipe::Write
// ============================================================================

Result<USIZE, Error> Pipe::Write(Span<const UINT8> data) noexcept
{
	// Validate the write end of the pipe
	if (writeFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pipe_WriteFailed);

	// System call to write to the pipe
	SSIZE result = System::Call(SYS_WRITE, (USIZE)writeFd, (USIZE)data.Data(), data.Size());
	// Validate the result of syscall
	if (result < 0)
	{
		return Result<USIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Pipe_WriteFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)result);
}

// ============================================================================
// Pipe::CloseRead
// ============================================================================

Result<VOID, Error> Pipe::CloseRead() noexcept
{
	if (readFd == INVALID_FD)
		return Result<VOID, Error>::Ok();

	System::Call(SYS_CLOSE, (USIZE)readFd);
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

	System::Call(SYS_CLOSE, (USIZE)writeFd);
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
