/**
 * @file shell_process.cc
 * @brief POSIX ShellProcess implementation (PTY-based)
 *
 * @details Spawns /bin/sh with a PTY for terminal-like I/O. Stdout and stderr
 * are merged through the PTY master fd.
 */

#include "platform/system/shell_process.h"
#include "logger.h"

// ============================================================================
// ShellProcess::EndOfLineChar
// ============================================================================

CHAR ShellProcess::EndOfLineChar() noexcept
{
	return '$';
}

// ============================================================================
// ShellProcess::Create
// ============================================================================

Result<ShellProcess, Error> ShellProcess::Create() noexcept
{
	auto ptyResult = Pty::Create();
	if (!ptyResult)
	{
		LOG_ERROR("Failed to create PTY for shell process: %e", ptyResult.Error());
		return Result<ShellProcess, Error>::Err(ptyResult, Error::ShellProcess_CreateFailed);
	}

	Pty &pty = ptyResult.Value();

	const CHAR *args[] = {"/bin/sh", nullptr};
	auto processResult = Process::Create("/bin/sh", args, pty.SlaveFd(), pty.SlaveFd(), pty.SlaveFd());
	(void)pty.CloseSlave();

	if (!processResult)
	{
		LOG_ERROR("Failed to spawn shell process: %e", processResult.Error());
		return Result<ShellProcess, Error>::Err(processResult, Error::ShellProcess_CreateFailed);
	}

	return Result<ShellProcess, Error>::Ok(
		ShellProcess(static_cast<Process &&>(processResult.Value()),
					 static_cast<Pty &&>(pty)));
}

// ============================================================================
// ShellProcess::Write
// ============================================================================

Result<USIZE, Error> ShellProcess::Write(const CHAR *data, USIZE length) noexcept
{
	return pty.Write(Span<const UINT8>((const UINT8 *)data, length));
}

// ============================================================================
// ShellProcess::Read
// ============================================================================

Result<USIZE, Error> ShellProcess::Read(CHAR *buffer, USIZE capacity) noexcept
{
	return pty.Read(Span<UINT8>((UINT8 *)buffer, capacity));
}

// ============================================================================
// ShellProcess::ReadError
// ============================================================================

Result<USIZE, Error> ShellProcess::ReadError([[maybe_unused]] CHAR *buffer, [[maybe_unused]] USIZE capacity) noexcept
{
	// PTY merges stdout and stderr — nothing separate to read
	return Result<USIZE, Error>::Ok(0);
}

// ============================================================================
// ShellProcess::Poll
// ============================================================================

SSIZE ShellProcess::Poll(SSIZE timeoutMs) noexcept
{
	return pty.Poll(timeoutMs);
}
