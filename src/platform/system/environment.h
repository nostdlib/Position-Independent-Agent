/**
 * @file environment.h
 * @brief Environment variable and platform information access
 *
 * @details Provides position-independent access to environment variables and
 * platform identification across all supported targets. On Windows, variables
 * are read from the PEB environment block. On Linux, macOS, and Solaris,
 * variables are retrieved from the process environ pointer. On UEFI,
 * GetVariable() always returns 0 as environment variables are not available.
 * No .rdata dependencies.
 *
 * Platform and system identification:
 * - GetAgentPlatform(): compile-time OS target string (e.g. "windows", "linux")
 * - GetOSVersion(): runtime OS version string (e.g. "Windows 10.0 Build 19045")
 * - GetHostname(): machine hostname from OS environment
 * - GetArchitecture(): compile-time CPU architecture string (e.g. "x86_64")
 */

#pragma once

#include "core/core.h"
#include "core/types/result.h"

/**
 * @class Environment
 * @brief Static class for environment variable and platform information access.
 */
class Environment
{
public:
	/**
	 * @brief Retrieves the value of an environment variable.
	 *
	 * @param name Variable name (null-terminated).
	 * @param buffer Output buffer to receive the value.
	 * @return Length of the value written, or 0 if not found.
	 *
	 * @note On UEFI, this always returns 0 (no environment variables).
	 */
	static USIZE GetVariable(const CHAR* name, Span<CHAR> buffer) noexcept;

	/**
	 * @brief Retrieves the compile-time OS target name.
	 *
	 * @param buffer Output buffer to receive the platform string.
	 * @return Length of the string written (excluding null terminator).
	 *
	 * @details Returns a short identifier for the OS the agent was compiled
	 * for (e.g. "windows", "linux", "macos", "android", "ios", "freebsd",
	 * "solaris", "uefi"). Determined at compile time from PLATFORM_* defines.
	 */
	static USIZE GetAgentPlatform(Span<CHAR> buffer) noexcept;

	/**
	 * @brief Retrieves the runtime OS version string.
	 *
	 * @param buffer Output buffer to receive the version string.
	 * @return Length of the string written (excluding null terminator).
	 *
	 * @details Returns a human-readable OS version string detected at runtime:
	 * - Windows: "Windows {Major}.{Minor} Build {Build}" from PEB fields
	 * - Linux/Android: "{sysname} {release}" from the uname syscall
	 * - macOS/iOS/FreeBSD: "{ostype} {osrelease}" from sysctl
	 * - Solaris: "{sysname} {release}" from utssys syscall
	 * - UEFI: "uefi"
	 *
	 * @return Ok(length) on success, Err on failure
	 */
	[[nodiscard]] static Result<USIZE, Error> GetOSVersion(Span<CHAR> buffer) noexcept;

	/**
	 * @brief Retrieves the machine hostname.
	 *
	 * @param buffer Output buffer to receive the hostname string.
	 * @return Length of the string written (excluding null terminator).
	 *
	 * @details Retrieves the hostname using platform-specific methods:
	 * - Windows: COMPUTERNAME environment variable from PEB
	 * - Linux/Android: HOSTNAME environment variable, fallback to /etc/hostname
	 * - macOS/FreeBSD/Solaris/iOS: HOSTNAME environment variable, fallback
	 *   to /etc/hostname
	 * - UEFI: returns 0 (no hostname concept)
	 *
	 * @return Ok(length) on success, Err on failure
	 */
	[[nodiscard]] static Result<USIZE, Error> GetHostname(Span<CHAR> buffer) noexcept;

	/**
	 * @brief Retrieves the compile-time CPU architecture string.
	 *
	 * @param buffer Output buffer to receive the architecture string.
	 * @return Length of the string written (excluding null terminator).
	 *
	 * @details Returns a short identifier for the CPU architecture the agent
	 * was compiled for (e.g. "x86_64", "aarch64", "i386", "armv7a", "riscv64",
	 * "riscv32", "mips64"). Determined at compile time from ARCHITECTURE_* defines.
	 */
	static USIZE GetArchitecture(Span<CHAR> buffer) noexcept;

	/**
	 * @brief Retrieves the value following a named flag in the OS command line.
	 *
	 * @param flag Flag name to search for (e.g. "--relay").
	 * @param buffer Output buffer to receive the value string.
	 * @return Length of the value written, or 0 if the flag was not found.
	 *
	 * @details Searches the process command line for the given flag and returns
	 * the next whitespace-delimited token as the value. On Windows, the command
	 * line is read from PEB->ProcessParameters->CommandLine. On Linux/Android,
	 * it is read from /proc/self/cmdline. On other platforms, returns 0.
	 */
	static USIZE GetCommandLineValue(const CHAR* flag, Span<CHAR> buffer) noexcept;

	// Prevent instantiation
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
};
