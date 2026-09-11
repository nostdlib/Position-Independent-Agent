/**
 * @file shell_process.h
 * @brief Cross-platform interactive shell process abstraction
 * @details Unified interface for spawning an interactive shell with I/O.
 * On POSIX platforms, uses PTY for merged stdout/stderr with terminal semantics.
 * On Windows, uses anonymous pipes with stderr merged into stdout (2 pipes).
 * Position-independent with no data section dependencies.
 * Part of the PLATFORM layer of the Position-Independent Runtime (PIR).
 *
 * Platform implementations:
 * - POSIX: PTY + Process (/bin/sh)
 * - Windows: 2x Pipe + Process (cmd.exe), stderr merged into stdout
 * - UEFI: Not supported (Create returns failure)
 */

#pragma once

#include "core/core.h"
#include "platform/system/process.h"
#include "platform/system/pipe.h"
#include "platform/system/pty.h"

/**
 * ShellProcess - RAII wrapper for an interactive shell with I/O
 *
 * Follows the Process/Pipe pattern: static factory, move-only, destructor cleans up.
 * Provides a unified Read/Write/Poll interface regardless of the underlying
 * I/O mechanism (PTY on POSIX, Pipes on Windows).
 */
class ShellProcess
{
public:
	/**
	 * EndOfLineChar - Platform-specific shell prompt character
	 *
	 * @return '>' on Windows (cmd.exe), '$' on POSIX (/bin/sh)
	 */
	static CHAR EndOfLineChar() noexcept;

	/**
	 * Create - Spawn an interactive shell process
	 *
	 * @details On POSIX, spawns /bin/sh via PTY. On Windows, spawns cmd.exe
	 * with stdin/stdout redirected through anonymous pipes and stderr merged
	 * into the stdout pipe (a single read drains both, matching the PTY),
	 * using CREATE_NO_WINDOW so no console window appears even when the host
	 * process has no console.
	 *
	 * @return ShellProcess on success, Error on failure
	 */
	[[nodiscard]] static Result<ShellProcess, Error> Create() noexcept;

	/**
	 * Write - Send input to the shell's stdin
	 *
	 * @param data Input data (UTF-8 bytes)
	 * @param length Number of bytes to write
	 * @return Number of bytes written on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Write(const CHAR *data, USIZE length) noexcept;

	/**
	 * Read - Read output from the shell's stdout
	 *
	 * @param buffer Buffer to read into
	 * @param capacity Buffer size in bytes
	 * @return Number of bytes read on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Read(CHAR *buffer, USIZE capacity) noexcept;

	/**
	 * Poll - Check if data is available for reading
	 *
	 * @param timeoutMs Maximum time to wait in milliseconds
	 * @return >0 if data available, 0 on timeout, <0 on error
	 */
	[[nodiscard]] SSIZE Poll(SSIZE timeoutMs) noexcept;

	~ShellProcess() noexcept = default;

	ShellProcess(ShellProcess &&other) noexcept = default;
	ShellProcess &operator=(ShellProcess &&) = delete;
	ShellProcess(const ShellProcess &) = delete;
	ShellProcess &operator=(const ShellProcess &) = delete;


private:
	Process process;

#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_UEFI)
	Pipe stdinPipe;
	Pipe stdoutPipe;

	ShellProcess(Process &&proc, Pipe &&inP, Pipe &&outP) noexcept
		: process(static_cast<Process &&>(proc)),
		  stdinPipe(static_cast<Pipe &&>(inP)),
		  stdoutPipe(static_cast<Pipe &&>(outP)) {}
#else
	Pty pty;

	ShellProcess(Process &&proc, Pty &&p) noexcept
		: process(static_cast<Process &&>(proc)),
		  pty(static_cast<Pty &&>(p)) {}
#endif
};
