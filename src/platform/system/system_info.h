/**
 * @file system_info.h
 * @brief System information retrieval
 *
 * @details Provides a cross-platform function to retrieve basic system
 * information including machine UUID, hostname, CPU architecture,
 * compile-time OS target (AgentPlatform), and runtime OS version (OSVersion).
 *
 * Platform implementations:
 * - Windows: UUID from SMBIOS, hostname from COMPUTERNAME environment variable,
 *   runtime OS version from the PEB (OSMajorVersion/OSMinorVersion/OSBuildNumber)
 * - Linux/Android: UUID from /etc/machine-id, hostname from HOSTNAME environment
 *   variable or /etc/hostname, runtime OS version from the uname syscall
 * - macOS/FreeBSD/Solaris/iOS: UUID from /etc/machine-id, hostname from
 *   HOSTNAME or /etc/hostname, runtime OS version from /proc/version or
 *   platform-specific files (best effort)
 * - UEFI: UUID unavailable, hostname unavailable (returns defaults)
 *
 * Architecture and AgentPlatform strings are determined at compile time from
 * the ARCHITECTURE_* and PLATFORM_* preprocessor defines. The OSVersion string
 * is populated at runtime via Environment::GetOSVersion().
 *
 * @ingroup platform
 */

#pragma once

#include "core/core.h"
#include "core/types/uuid.h"

/**
 * @brief System information structure
 *
 * @details Contains identifying information about the host system.
 * Hostname, Architecture, AgentPlatform, and OSVersion are null-terminated
 * narrow strings.
 */
#pragma pack(push, 1)
struct SystemInfo
{
    UUID MachineUUID;        ///< Machine-unique identifier (hardware/OS level)
    CHAR Hostname[256];      ///< Machine hostname / computer name
    CHAR Architecture[32];   ///< CPU architecture (e.g. "x86_64", "aarch64")
    CHAR AgentPlatform[32];  ///< Compile-time OS target (e.g. "windows", "linux")
    CHAR OSVersion[128];     ///< Runtime OS version (e.g. "Windows 10.0 Build 19045", "Linux 6.1.0")
};
#pragma pack(pop)

/**
 * @brief Retrieves system information for the current host.
 *
 * @details Populates a SystemInfo structure with:
 * - MachineUUID: Hardware/OS-level unique identifier (platform-specific)
 * - Hostname: Retrieved from OS environment (platform-specific)
 * - Architecture: Compile-time string from ARCHITECTURE_* define
 * - AgentPlatform: Compile-time string from PLATFORM_* define
 * - OSVersion: Runtime OS version string (e.g. "Windows 10.0 Build 19045")
 *
 * @param[out] info Pointer to SystemInfo structure to populate.
 *                  The structure is zeroed before populating.
 */
VOID GetSystemInfo(SystemInfo *info);
