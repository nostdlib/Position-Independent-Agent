/**
 * @file syscall.h
 * @brief FreeBSD syscall numbers and BSD type definitions.
 *
 * @details Defines FreeBSD syscall numbers, POSIX/BSD constants, file descriptor
 * flags, memory protection flags, socket options, errno values, and kernel
 * structures (FreeBsdDirent, Timespec, Pollfd). Syscall numbers are shared
 * across i386, x86_64, and AArch64 FreeBSD architectures. Many constants
 * match macOS (shared BSD heritage) but some differ (AT_FDCWD, O_DIRECTORY,
 * CLOCK_MONOTONIC, AF_INET6).
 *
 * @see FreeBSD syscalls.master
 *      https://cgit.freebsd.org/src/tree/sys/kern/syscalls.master
 */
#pragma once

#include "core/types/primitives.h"

// File I/O
constexpr USIZE SYS_EXIT       = 1;
constexpr USIZE SYS_FORK       = 2;
constexpr USIZE SYS_READ       = 3;
constexpr USIZE SYS_WRITE      = 4;
constexpr USIZE SYS_OPEN       = 5;
constexpr USIZE SYS_CLOSE      = 6;
constexpr USIZE SYS_LSEEK      = 478;

// Device I/O
constexpr USIZE SYS_IOCTL      = 54;

// File operations
constexpr USIZE SYS_STAT       = 188;
constexpr USIZE SYS_FSTAT      = 551;
constexpr USIZE SYS_UNLINK     = 10;

// Directory operations
constexpr USIZE SYS_MKDIR      = 136;
constexpr USIZE SYS_RMDIR      = 137;
constexpr USIZE SYS_GETDIRENTRIES = 554;

// *at syscalls
constexpr USIZE SYS_OPENAT     = 499;
constexpr USIZE SYS_UNLINKAT   = 503;
constexpr USIZE SYS_MKDIRAT    = 496;
constexpr USIZE SYS_FSTATAT    = 552;
constexpr SSIZE AT_FDCWD       = -100;
constexpr INT32 AT_REMOVEDIR   = 0x0800;

// Memory operations
constexpr USIZE SYS_MMAP       = 477;
constexpr USIZE SYS_MUNMAP     = 73;

// Socket operations
constexpr USIZE SYS_SOCKET     = 97;
constexpr USIZE SYS_CONNECT    = 98;
constexpr USIZE SYS_BIND       = 104;
constexpr USIZE SYS_SENDTO     = 133;
constexpr USIZE SYS_RECVFROM   = 29;
constexpr USIZE SYS_SHUTDOWN   = 134;
constexpr USIZE SYS_SETSOCKOPT = 105;
constexpr USIZE SYS_GETSOCKOPT = 118;
constexpr USIZE SYS_FCNTL      = 92;
constexpr USIZE SYS_POLL       = 209;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME  = 232;
constexpr USIZE SYS_GETTIMEOFDAY   = 116;

// Process operations
constexpr USIZE SYS_EXECVE     = 59;
constexpr USIZE SYS_DUP2       = 90;
constexpr USIZE SYS_SETSID     = 147;
constexpr USIZE SYS_WAIT4      = 7;
constexpr USIZE SYS_KILL       = 37;
constexpr USIZE SYS_PIPE       = 42;

// PTY operations
constexpr USIZE SYS_POSIX_OPENPT = 504;

// =============================================================================
// POSIX/BSD Constants
// =============================================================================

// Standard file descriptors
constexpr INT32 STDIN_FILENO  = 0;
constexpr INT32 STDOUT_FILENO = 1;
constexpr INT32 STDERR_FILENO = 2;

// File open flags (BSD values -- differ from Linux!)
constexpr INT32 O_RDONLY    = 0x0000;
constexpr INT32 O_WRONLY    = 0x0001;
constexpr INT32 O_RDWR      = 0x0002;
constexpr INT32 O_NONBLOCK  = 0x0004;
constexpr INT32 O_APPEND    = 0x0008;
constexpr INT32 O_CREAT     = 0x0200;
constexpr INT32 O_TRUNC     = 0x0400;
constexpr INT32 O_DIRECTORY  = 0x00020000;

// lseek whence values
constexpr INT32 SEEK_SET = 0;
constexpr INT32 SEEK_CUR = 1;
constexpr INT32 SEEK_END = 2;

// File mode/permission bits (same as POSIX)
constexpr INT32 S_IRUSR = 0x0100;  // User read
constexpr INT32 S_IWUSR = 0x0080;  // User write
constexpr INT32 S_IXUSR = 0x0040;  // User execute
constexpr INT32 S_IRGRP = 0x0020;  // Group read
constexpr INT32 S_IWGRP = 0x0010;  // Group write
constexpr INT32 S_IXGRP = 0x0008;  // Group execute
constexpr INT32 S_IROTH = 0x0004;  // Others read
constexpr INT32 S_IWOTH = 0x0002;  // Others write
constexpr INT32 S_IXOTH = 0x0001;  // Others execute

// Directory entry types (same as BSD)
constexpr UINT8 DT_UNKNOWN = 0;
constexpr UINT8 DT_FIFO    = 1;
constexpr UINT8 DT_CHR     = 2;
constexpr UINT8 DT_DIR     = 4;
constexpr UINT8 DT_BLK     = 6;
constexpr UINT8 DT_REG     = 8;
constexpr UINT8 DT_LNK     = 10;
constexpr UINT8 DT_SOCK    = 12;

// Memory protection flags (same as POSIX)
constexpr INT32 PROT_READ  = 0x01;
constexpr INT32 PROT_WRITE = 0x02;
constexpr INT32 PROT_EXEC  = 0x04;

// Memory mapping flags (BSD values)
constexpr INT32 MAP_SHARED    = 0x0001;
constexpr INT32 MAP_PRIVATE   = 0x0002;
constexpr INT32 MAP_ANONYMOUS = 0x1000;
#define MAP_FAILED ((PVOID)(-1))

// Socket options (BSD values -- same as macOS)
constexpr INT32 SOL_SOCKET   = 0xFFFF;
constexpr INT32 SO_ERROR     = 0x1007;
constexpr INT32 SO_RCVTIMEO  = 0x1006;
constexpr INT32 SO_SNDTIMEO  = 0x1005;
constexpr INT32 IPPROTO_TCP  = 6;
constexpr INT32 TCP_NODELAY  = 1;

// fcntl commands
constexpr INT32 F_GETFL = 3;
constexpr INT32 F_SETFL = 4;

// errno values
constexpr INT32 EEXIST = 17;
constexpr INT32 EINPROGRESS = 36;

// poll event flags
constexpr INT16 POLLIN  = 0x0001;
constexpr INT16 POLLOUT = 0x0004;
constexpr INT16 POLLERR = 0x0008;
constexpr INT16 POLLHUP = 0x0010;

// PTY flags
constexpr INT32 O_NOCTTY  = 0x8000;
constexpr INT32 O_CLOEXEC = 0x100000;

// Clock IDs
constexpr INT32 CLOCK_REALTIME  = 0;
constexpr INT32 CLOCK_MONOTONIC = 4;

// Signal numbers
constexpr INT32 SIGKILL = 9;

// wait4 options
constexpr INT32 WNOHANG = 1;

// Invalid file descriptor
constexpr SSIZE INVALID_FD = -1;

// =============================================================================
// FreeBSD Structures
// =============================================================================

/// @brief FreeBSD directory entry returned by the getdirentries syscall (FreeBSD 12+ ABI).
struct FreeBsdDirent
{
	UINT64 Fileno; ///< Inode number
	UINT64 Off;    ///< Directory offset of next entry
	UINT16 Reclen; ///< Total size of this record in bytes (including padding)
	UINT8 Type;    ///< File type (DT_REG, DT_DIR, DT_LNK, etc.)
	UINT8 Pad0;    ///< Padding
	UINT16 Namlen; ///< Length of the filename in bytes (excluding null terminator)
	UINT16 Pad1;   ///< Padding
	CHAR Name[];   ///< Null-terminated filename
};

/// @brief POSIX timespec with nanosecond precision, used for clock_gettime.
struct Timespec
{
	SSIZE Sec;  ///< Seconds since the Unix epoch (1970-01-01T00:00:00Z)
	SSIZE Nsec; ///< Nanoseconds (0 to 999,999,999)
};

/// @brief File descriptor entry for the poll syscall.
struct Pollfd
{
	INT32 Fd;      ///< File descriptor to monitor
	INT16 Events;  ///< Requested event bitmask (e.g., POLLOUT)
	INT16 Revents; ///< Returned event bitmask filled by the kernel (e.g., POLLERR, POLLHUP)
};
