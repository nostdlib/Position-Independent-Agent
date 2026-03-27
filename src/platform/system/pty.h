/**
 * @file pty.h
 * @brief Cross-platform pseudo-terminal (PTY) abstraction
 * @details RAII PTY with create, read, write, poll, and close operations.
 * Position-independent with no data section dependencies.
 * Part of the PLATFORM layer of the Position-Independent Runtime (PIR).
 *
 * Platform implementations:
 * - POSIX: /dev/ptmx open, unlock, slave path discovery, read/write/poll syscalls
 * - Windows: Not supported (all operations return failure)
 * - UEFI: Not supported (all operations return failure)
 */

#pragma once

#include "core/core.h"

/**
 * Pty - RAII wrapper for a pseudo-terminal master/slave pair
 *
 * Follows the Pipe pattern: static factory, move-only, destructor cleans up.
 */
class Pty
{
public:
	/**
	 * Create - Create a pseudo-terminal pair (master + slave)
	 *
	 * @return Pty with master and slave fds on success, Error on failure
	 */
	[[nodiscard]] static Result<Pty, Error> Create() noexcept;

	/**
	 * Read - Read data from the master end of the PTY
	 *
	 * @param buffer Buffer to read into
	 * @return Number of bytes read on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Read(Span<UINT8> buffer) noexcept;

	/**
	 * Write - Write data to the master end of the PTY
	 *
	 * @param data Data to write
	 * @return Number of bytes written on success, Error on failure
	 */
	[[nodiscard]] Result<USIZE, Error> Write(Span<const UINT8> data) noexcept;

	/**
	 * Poll - Check if data is available for reading on the master fd
	 *
	 * @param timeoutMs Maximum time to wait in milliseconds
	 * @return >0 if data available, 0 on timeout, <0 on error
	 */
	[[nodiscard]] SSIZE Poll(SSIZE timeoutMs) noexcept;

	/**
	 * SlaveFd - Get the slave file descriptor for passing to Process::Create
	 *
	 * @return Slave fd, or -1 if already closed
	 */
	[[nodiscard]] SSIZE SlaveFd() const noexcept { return slaveFd; }

	/**
	 * MasterFd - Get the master file descriptor
	 *
	 * @return Master fd, or -1 if invalid
	 */
	[[nodiscard]] SSIZE MasterFd() const noexcept { return masterFd; }

	/**
	 * CloseSlave - Close the slave end (call after Process::Create in parent)
	 *
	 * @return void on success, Error on failure
	 */
	Result<VOID, Error> CloseSlave() noexcept;

	/**
	 * IsValid - Check if the master end is open
	 *
	 * @return true if master fd is valid
	 */
	[[nodiscard]] BOOL IsValid() const noexcept { return masterFd != INVALID_FD; }

	/// @name RAII
	/// @{
	~Pty() noexcept
	{
		if (IsValid())
			(VOID)Close();
	}

	Pty(Pty &&other) noexcept
		: masterFd(other.masterFd), slaveFd(other.slaveFd)
	{
		other.masterFd = INVALID_FD;
		other.slaveFd = INVALID_FD;
	}

	Pty &operator=(Pty &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			masterFd = other.masterFd;
			slaveFd = other.slaveFd;
			other.masterFd = INVALID_FD;
			other.slaveFd = INVALID_FD;
		}
		return *this;
	}

	Pty(const Pty &) = delete;
	Pty &operator=(const Pty &) = delete;
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

	SSIZE masterFd;
	SSIZE slaveFd;

	Pty(SSIZE master, SSIZE slave) noexcept
		: masterFd(master), slaveFd(slave)
	{
	}

	Result<VOID, Error> Close() noexcept;
};
