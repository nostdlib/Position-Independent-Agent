/**
 * @file syscall.h
 * @brief Linux syscall numbers and POSIX type definitions.
 *
 * @details Includes the architecture-specific syscall number header for the
 * current build target and defines POSIX/Linux constants, file descriptor
 * flags, memory protection flags, socket options, errno values, and kernel
 * structures (dirent64, timespec, timeval, pollfd) shared across all
 * Linux architectures.
 */
#pragma once

#include "core/types/primitives.h"

#if defined(ARCHITECTURE_X86_64)
#include "platform/kernel/linux/syscall.x86_64.h"
#elif defined(ARCHITECTURE_I386)
#include "platform/kernel/linux/syscall.i386.h"
#elif defined(ARCHITECTURE_AARCH64)
#include "platform/kernel/linux/syscall.aarch64.h"
#elif defined(ARCHITECTURE_ARMV7A)
#include "platform/kernel/linux/syscall.armv7a.h"
#elif defined(ARCHITECTURE_RISCV64)
#include "platform/kernel/linux/syscall.riscv64.h"
#elif defined(ARCHITECTURE_RISCV32)
#include "platform/kernel/linux/syscall.riscv32.h"
#elif defined(ARCHITECTURE_MIPS64)
#include "platform/kernel/linux/syscall.mips64.h"
#else
#error "Unsupported architecture"
#endif

// =============================================================================
// POSIX/Linux Constants
// =============================================================================

// Standard file descriptors
constexpr INT32 STDIN_FILENO = 0;
constexpr INT32 STDOUT_FILENO = 1;
constexpr INT32 STDERR_FILENO = 2;

// File open flags
constexpr INT32 O_RDONLY = 0x0000;
constexpr INT32 O_WRONLY = 0x0001;
constexpr INT32 O_RDWR = 0x0002;
#if defined(ARCHITECTURE_MIPS64)
// MIPS Linux inherited IRIX/SVR4 values which differ from the generic ABI
constexpr INT32 O_CREAT = 0x0100;
constexpr INT32 O_TRUNC = 0x0200;
constexpr INT32 O_APPEND = 0x0008;
constexpr INT32 O_NONBLOCK = 0x0080;
#else
constexpr INT32 O_CREAT = 0x0040;
constexpr INT32 O_TRUNC = 0x0200;
constexpr INT32 O_APPEND = 0x0400;
constexpr INT32 O_NONBLOCK = 0x0800;
#endif

#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_MIPS64)
constexpr INT32 O_DIRECTORY = 0x10000;
#elif defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_ARMV7A) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
constexpr INT32 O_DIRECTORY = 0x4000;
#endif

// lseek whence values
constexpr INT32 SEEK_SET = 0;
constexpr INT32 SEEK_CUR = 1;
constexpr INT32 SEEK_END = 2;

// File mode/permission bits
constexpr INT32 S_IRUSR = 0x0100;  // User read
constexpr INT32 S_IWUSR = 0x0080;  // User write
constexpr INT32 S_IXUSR = 0x0040;  // User execute
constexpr INT32 S_IRGRP = 0x0020;  // Group read
constexpr INT32 S_IWGRP = 0x0010;  // Group write
constexpr INT32 S_IXGRP = 0x0008;  // Group execute
constexpr INT32 S_IROTH = 0x0004;  // Others read
constexpr INT32 S_IWOTH = 0x0002;  // Others write
constexpr INT32 S_IXOTH = 0x0001;  // Others execute

// Directory entry types
constexpr UINT8 DT_UNKNOWN = 0;
constexpr UINT8 DT_FIFO = 1;
constexpr UINT8 DT_CHR = 2;
constexpr UINT8 DT_DIR = 4;
constexpr UINT8 DT_BLK = 6;
constexpr UINT8 DT_REG = 8;
constexpr UINT8 DT_LNK = 10;
constexpr UINT8 DT_SOCK = 12;

// Memory protection flags
constexpr INT32 PROT_READ = 0x01;
constexpr INT32 PROT_WRITE = 0x02;
constexpr INT32 PROT_EXEC = 0x04;

// Memory mapping flags
constexpr INT32 MAP_SHARED = 0x01;
constexpr INT32 MAP_PRIVATE = 0x02;
#if defined(ARCHITECTURE_MIPS64)
constexpr INT32 MAP_ANONYMOUS = 0x0800;
#else
constexpr INT32 MAP_ANONYMOUS = 0x20;
#endif
#define MAP_FAILED ((PVOID)(-1))

// Clock IDs
constexpr INT32 CLOCK_REALTIME = 0;
constexpr INT32 CLOCK_MONOTONIC = 1;

// Socket options
#if defined(ARCHITECTURE_MIPS64)
// MIPS Linux inherited IRIX/SVR4 socket option values
constexpr INT32 SOL_SOCKET = 0xFFFF;
constexpr INT32 SO_ERROR = 0x1007;
constexpr INT32 SO_RCVTIMEO = 0x1006;
constexpr INT32 SO_SNDTIMEO = 0x1005;
#elif defined(ARCHITECTURE_RISCV32)
constexpr INT32 SOL_SOCKET = 1;
constexpr INT32 SO_ERROR = 4;
// riscv32 has no time32 support — SO_RCVTIMEO_OLD/SO_SNDTIMEO_OLD (20/21) do
// not exist; the kernel only provides the _NEW variants (66/67) which expect
// 64-bit timeval fields.
constexpr INT32 SO_RCVTIMEO = 66;
constexpr INT32 SO_SNDTIMEO = 67;
#else
constexpr INT32 SOL_SOCKET = 1;
constexpr INT32 SO_ERROR = 4;
constexpr INT32 SO_RCVTIMEO = 20;
constexpr INT32 SO_SNDTIMEO = 21;
#endif
constexpr INT32 IPPROTO_TCP = 6;
constexpr INT32 TCP_NODELAY = 1;

// fcntl commands
constexpr INT32 F_GETFL = 3;
constexpr INT32 F_SETFL = 4;

// errno values
constexpr INT32 EEXIST = 17;
#if defined(ARCHITECTURE_MIPS64)
constexpr INT32 EINPROGRESS = 150;
#else
constexpr INT32 EINPROGRESS = 115;
#endif

// poll event flags
constexpr INT16 POLLIN  = 0x0001;
constexpr INT16 POLLOUT = 0x0004;
constexpr INT16 POLLERR = 0x0008;
constexpr INT16 POLLHUP = 0x0010;

// PTY flags
#if defined(ARCHITECTURE_MIPS64)
constexpr INT32 O_NOCTTY  = 0x800;
#else
constexpr INT32 O_NOCTTY  = 0x100;
#endif
constexpr INT32 O_CLOEXEC = 0x80000;

// Signal numbers
constexpr INT32 SIGKILL = 9;

// wait4 options
constexpr INT32 WNOHANG = 1;

// Invalid file descriptor
constexpr SSIZE INVALID_FD = -1;

// =============================================================================
// Linux Structures
// =============================================================================

/// @brief POSIX utsname structure returned by the uname syscall.
/// @details Contains system identification strings: OS name, hostname,
/// kernel release, version, and machine architecture. Each field is 65 bytes
/// on Linux (including null terminator). The domainname field is a Linux
/// extension (not in POSIX).
struct Utsname
{
	CHAR Sysname[65];    ///< OS name (e.g. "Linux")
	CHAR Nodename[65];   ///< Network hostname
	CHAR Release[65];    ///< Kernel release (e.g. "6.1.0")
	CHAR Version[65];    ///< Kernel version string
	CHAR Machine[65];    ///< Hardware architecture (e.g. "x86_64")
	CHAR Domainname[65]; ///< NIS/YP domain name (Linux extension)
};

/// @brief Linux directory entry returned by the getdents64 syscall.
struct LinuxDirent64
{
	UINT64 Ino;    ///< Inode number
	INT64 Off;     ///< Offset to the next entry in the directory stream
	UINT16 Reclen; ///< Total size of this record in bytes (including padding)
	UINT8 Type;    ///< File type (DT_REG, DT_DIR, DT_LNK, etc.)
	CHAR Name[];   ///< Null-terminated filename
};

/// @brief POSIX time specification with nanosecond precision.
/// @details On RISC-V 32-bit, the Linux kernel was designed without support for
/// legacy 32-bit time syscalls (__ARCH_WANT_TIME32_SYSCALLS is not defined).
/// All time-related syscalls (clock_gettime, ppoll, etc.) require 64-bit time
/// fields, matching the kernel's __kernel_timespec layout.
struct Timespec
{
#if defined(ARCHITECTURE_RISCV32)
	INT64 Sec;  ///< Seconds (64-bit required on riscv32)
	INT64 Nsec; ///< Nanoseconds (64-bit required on riscv32)
#else
	SSIZE Sec;  ///< Seconds since the Unix epoch (1970-01-01T00:00:00Z)
	SSIZE Nsec; ///< Nanoseconds (0 to 999,999,999)
#endif
};

/// @brief POSIX time value with microsecond precision, used for socket timeouts.
/// @details On RISC-V 32-bit, fields must be 64-bit to match the kernel ABI.
struct Timeval
{
#if defined(ARCHITECTURE_RISCV32)
	INT64 Sec;  ///< Seconds (64-bit required on riscv32)
	INT64 Usec; ///< Microseconds (64-bit required on riscv32)
#else
	SSIZE Sec;  ///< Seconds since the Unix epoch (1970-01-01T00:00:00Z)
	SSIZE Usec; ///< Microseconds (0 to 999,999)
#endif
};

/// @brief File descriptor entry for the poll/ppoll syscall.
struct Pollfd
{
	INT32 Fd;      ///< File descriptor to monitor
	INT16 Events;  ///< Requested event bitmask (e.g., POLLOUT)
	INT16 Revents; ///< Returned event bitmask filled by the kernel (e.g., POLLERR, POLLHUP)
};
