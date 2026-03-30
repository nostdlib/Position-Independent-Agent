/**
 * @file pipe.h
 * @brief Cross-platform anonymous pipe for inter-process communication
 * @details RAII pipe with create, read, write, and per-end close operations.
 * Position-independent with no data section dependencies.
 * Part of the PLATFORM layer of the Position-Independent Runtime (PIR).
 *
 * Typical usage with Process:
 * @code
 *   auto pipe = Pipe::Create();
 *   auto proc = Process::Create(cmd, args, -1, pipe.Value().WriteEnd(), -1);
 *   pipe.Value().CloseWrite();  // close write end in parent
 *   pipe.Value().Read(buffer);  // read child's stdout
 * @endcode
 *
 * Platform implementations:
 * - POSIX: pipe() / pipe2() syscall, read/write/close syscalls
 * - Windows: Kernel32 CreatePipe, ZwReadFile/ZwWriteFile/ZwClose
 * - UEFI: Not supported (all operations return failure)
 */

#pragma once

#include "core/core.h"

/**
 * Pipe - RAII wrapper for an anonymous pipe (read/write fd pair)
 *
 * Follows the Socket pattern: static factory, move-only, destructor cleans up.
 */
class Pipe
{
public:
	/**
	 * Create - Create an anonymous pipe
	 *
	 * @return Pipe with read and write ends on success, Error on failure
	 */
	[[nodiscard]] static Result<Pipe, Error> Create() noexcept;

	/**
	 * Read - Read data from the read end of the pipe
	 *
	 * @param buffer Buffer to read into
	 * @return Number of bytes read on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Read(Span<UINT8> buffer) noexcept;

	/**
	 * Write - Write data to the write end of the pipe
	 *
	 * @param data Data to write
	 * @return Number of bytes written on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Write(Span<const UINT8> data) noexcept;

	/**
	 * ReadEnd - Get the read end file descriptor/handle
	 *
	 * @return Read fd on POSIX, read handle (as SSIZE) on Windows, -1 if closed
	 */
	[[nodiscard]] SSIZE ReadEnd() const noexcept { return readFd; }

	/**
	 * WriteEnd - Get the write end file descriptor/handle
	 *
	 * @return Write fd on POSIX, write handle (as SSIZE) on Windows, -1 if closed
	 */
	[[nodiscard]] SSIZE WriteEnd() const noexcept { return writeFd; }

	/**
	 * CloseRead - Close the read end of the pipe
	 *
	 * @return void on success, Error on failure
	 */
	Result<VOID, Error> CloseRead() noexcept;

	/**
	 * CloseWrite - Close the write end of the pipe
	 *
	 * @return void on success, Error on failure
	 */
	Result<VOID, Error> CloseWrite() noexcept;

	/**
	 * IsValid - Check if at least one end of the pipe is open
	 *
	 * @return true if either end is valid
	 */
	[[nodiscard]] BOOL IsValid() const noexcept { return readFd != INVALID_FD || writeFd != INVALID_FD; }

	/// @name RAII
	/// @{
	~Pipe() noexcept
	{
		if (IsValid())
			(VOID)Close();
	}

	Pipe(Pipe &&other) noexcept
		: readFd(other.readFd), writeFd(other.writeFd)
	{
		other.readFd = INVALID_FD;
		other.writeFd = INVALID_FD;
	}

	Pipe &operator=(Pipe &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			readFd = other.readFd;
			writeFd = other.writeFd;
			other.readFd = INVALID_FD;
			other.writeFd = INVALID_FD;
		}
		return *this;
	}

	Pipe(const Pipe &) = delete;
	Pipe &operator=(const Pipe &) = delete;
	/// @}

	/// @name Stack-Only Enforcement
	/// @{
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
	/// @}

private:
	static constexpr SSIZE INVALID_FD = -1;

	SSIZE readFd;
	SSIZE writeFd;

	Pipe(SSIZE read, SSIZE write) noexcept
		: readFd(read), writeFd(write)
	{
	}

	Result<VOID, Error> Close() noexcept;
};
