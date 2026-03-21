/**
 * @file syscall.armv7a.h
 * @brief ARMv7-A Linux syscall number definitions.
 *
 * @details Defines the syscall number constants for the ARMv7-A (ARM EABI) Linux
 * kernel ABI. Includes both direct socket syscalls and legacy socketcall
 * multiplexer constants for compatibility. Uses SYS_MMAP2 and SYS_FCNTL64
 * variants specific to the 32-bit ARM architecture.
 */
#pragma once

#include "core/types/primitives.h"

// File I/O
constexpr USIZE SYS_READ = 3;
constexpr USIZE SYS_WRITE = 4;
constexpr USIZE SYS_OPEN = 5;
constexpr USIZE SYS_CLOSE = 6;
constexpr USIZE SYS_LSEEK = 19;
constexpr USIZE SYS_OPENAT = 322;

// Device I/O
constexpr USIZE SYS_IOCTL = 54;

// File operations
constexpr USIZE SYS_STAT = 106;
constexpr USIZE SYS_FSTAT = 108;
constexpr USIZE SYS_FSTATAT64 = 327;
constexpr USIZE SYS_UNLINK = 10;

// Directory operations
constexpr USIZE SYS_MKDIR = 39;
constexpr USIZE SYS_RMDIR = 40;
constexpr USIZE SYS_GETDENTS64 = 217;  // ARM EABI uses 217, not 220

// Memory operations
constexpr USIZE SYS_MMAP2 = 192;
constexpr USIZE SYS_MUNMAP = 91;

// Socket operations
constexpr USIZE SYS_SOCKET = 281;
constexpr USIZE SYS_BIND = 282;
constexpr USIZE SYS_CONNECT = 283;
constexpr USIZE SYS_SENDTO = 290;
constexpr USIZE SYS_RECVFROM = 292;
constexpr USIZE SYS_SHUTDOWN = 293;
constexpr USIZE SYS_SETSOCKOPT = 294;
constexpr USIZE SYS_GETSOCKOPT = 295;
constexpr USIZE SYS_PPOLL = 336;
constexpr USIZE SYS_FCNTL64 = 221;

// Legacy socketcall support (for compatibility)
constexpr USIZE SYS_SOCKETCALL = 102;
constexpr INT32 SOCKOP_SOCKET_ARM = 1;
constexpr INT32 SOCKOP_BIND_ARM = 2;
constexpr INT32 SOCKOP_CONNECT_ARM = 3;
constexpr INT32 SOCKOP_SEND_ARM = 9;
constexpr INT32 SOCKOP_RECV_ARM = 10;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 263;

// Random operations
constexpr USIZE SYS_GETRANDOM = 384;

// System information
constexpr USIZE SYS_UNAME = 122;

// Process operations
constexpr USIZE SYS_EXIT = 1;
constexpr USIZE SYS_EXIT_GROUP = 248;
constexpr USIZE SYS_FORK = 2;
constexpr USIZE SYS_EXECVE = 11;
constexpr USIZE SYS_DUP2 = 63;
constexpr USIZE SYS_WAIT4 = 114;
constexpr USIZE SYS_KILL = 37;
constexpr USIZE SYS_SETSID = 66;
constexpr USIZE SYS_PIPE = 42;
