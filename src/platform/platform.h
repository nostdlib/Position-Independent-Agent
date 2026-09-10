/**
 * @file platform.h
 * @brief Platform Abstraction Layer aggregate header
 *
 * @details Includes all platform-layer headers for OS and hardware abstraction.
 * This is the single entry point for consumers of the PLATFORM layer, pulling in
 * memory allocation, system utilities, I/O services, networking, and process
 * management. Platform-specific headers (UEFI, Windows, Linux, macOS, Solaris, FreeBSD)
 * are conditionally included based on the target platform. Depends on CORE.
 *
 * @defgroup platform Platform Abstraction Layer
 * @{
 */

#pragma once

#include "core/core.h"

// =============================================================================
// Platform-Specific Headers
// =============================================================================

#if defined(PLATFORM_UEFI)
// UEFI platform - include EFI types and system table
#include "platform/kernel/uefi/efi_types.h"
#include "platform/kernel/uefi/efi_system_table.h"
#include "platform/kernel/uefi/efi_context.h"
#endif

// =============================================================================
// Platform Core
// =============================================================================

// Cross-platform exit process function
NO_RETURN VOID ExitProcess(USIZE code);

// =============================================================================
// Platform Services
// =============================================================================


#include "platform/memory/allocator.h"

#include "platform/system/date_time.h"
#include "platform/system/random.h"
#include "platform/system/machine_id.h"

#include "platform/console/console.h"
#include "platform/fs/offset_origin.h"
#include "platform/fs/directory_entry.h"
#include "platform/fs/file.h"
#include "platform/fs/directory.h"
#include "platform/fs/directory_iterator.h"
#include "platform/fs/path.h"
#include "platform/console/logger.h"

#include "platform/socket/socket.h"

#include "platform/screen/screen.h"

#include "platform/system/pipe.h"
#include "platform/system/process.h"
#include "platform/system/pty.h"
#include "platform/system/shell_process.h"

/** @} */ // end of platform group
