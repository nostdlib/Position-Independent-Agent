/**
 * @file process.h
 * @brief Cross-platform process creation and management
 * @details RAII process handle with create, wait, terminate, and optional
 * I/O redirection. Position-independent with no data section dependencies.
 * Part of the PLATFORM layer of the Position-Independent Runtime (PIR).
 *
 * Platform implementations:
 * - POSIX: fork + execve, wait4, kill
 * - Windows: CreateProcessW, ZwWaitForSingleObject, ZwTerminateProcess
 * - UEFI: Not supported (all operations return failure)
 */

#pragma once

#include "core/core.h"

/**
 * Process - RAII wrapper for a spawned child process
 *
 * Follows the Socket pattern: static factory, move-only, destructor cleans up.
 */
class Process
{
public:
	/**
	 * Create - Spawn a new process
	 *
	 * @param path Full path to executable
	 * @param args Null-terminated argument array (first element = program name)
	 * @param stdinFd File descriptor/handle for child's stdin (-1 to inherit parent's)
	 * @param stdoutFd File descriptor/handle for child's stdout (-1 to inherit parent's)
	 * @param stderrFd File descriptor/handle for child's stderr (-1 to inherit parent's)
	 * @return Process handle on success, Error on failure
	 */
	[[nodiscard]] static Result<Process, Error> Create(
		const CHAR *path,
		const CHAR *const args[],
		SSIZE stdinFd = -1,
		SSIZE stdoutFd = -1,
		SSIZE stderrFd = -1) noexcept;

	/**
	 * Wait - Block until the process exits
	 *
	 * @return Exit code on success, Error on failure
	 */
	[[nodiscard]] Result<SSIZE, Error> Wait() noexcept;

	/**
	 * Terminate - Forcefully kill the process
	 *
	 * @return void on success, Error on failure
	 */
	[[nodiscard]] Result<VOID, Error> Terminate() noexcept;

	/**
	 * IsRunning - Check if the process is still alive
	 *
	 * @return true if running, false if exited or invalid
	 */
	[[nodiscard]] BOOL IsRunning() const noexcept;

	/**
	 * Id - Get the process identifier
	 *
	 * @return PID on POSIX, process ID on Windows
	 */
	[[nodiscard]] SSIZE Id() const noexcept { return id; }

	/**
	 * IsValid - Check if this Process holds a valid handle
	 *
	 * @return true if valid
	 */
	[[nodiscard]] BOOL IsValid() const noexcept { return id != INVALID_ID; }

	/// @name RAII
	/// @{
	~Process() noexcept
	{
		if (IsValid())
			(VOID)Close();
	}

	Process(Process &&other) noexcept
		: id(other.id)
#if defined(PLATFORM_WINDOWS)
		, handle(other.handle)
#endif
	{
		other.id = INVALID_ID;
#if defined(PLATFORM_WINDOWS)
		other.handle = nullptr;
#endif
	}

	Process &operator=(Process &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			id = other.id;
#if defined(PLATFORM_WINDOWS)
			handle = other.handle;
			other.handle = nullptr;
#endif
			other.id = INVALID_ID;
		}
		return *this;
	}

	Process(const Process &) = delete;
	Process &operator=(const Process &) = delete;
	/// @}

	/// @name Stack-Only Enforcement
	/// @{
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
	/// @}

private:
	static constexpr SSIZE INVALID_ID = -1;

	SSIZE id;
#if defined(PLATFORM_WINDOWS)
	PVOID handle;
#endif

	explicit Process(SSIZE processId) noexcept
		: id(processId)
#if defined(PLATFORM_WINDOWS)
		, handle(nullptr)
#endif
	{
	}

#if defined(PLATFORM_WINDOWS)
	Process(SSIZE processId, PVOID processHandle) noexcept
		: id(processId), handle(processHandle)
	{
	}
#endif

	Result<VOID, Error> Close() noexcept;
};
