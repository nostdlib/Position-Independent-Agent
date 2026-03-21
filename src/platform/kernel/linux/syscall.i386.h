/**
 * @file syscall.i386.h
 * @brief i386 Linux syscall number definitions.
 *
 * @details Defines the syscall number constants for the i386 Linux kernel ABI.
 * Notable differences from x86_64 include the use of SYS_MMAP2 instead of
 * SYS_MMAP and the SYS_SOCKETCALL multiplexer with SOCKOP_* subcodes instead
 * of individual socket syscalls.
 */
#pragma once

#include "core/types/primitives.h"

// File I/O
constexpr USIZE SYS_READ = 3;
constexpr USIZE SYS_WRITE = 4;
constexpr USIZE SYS_OPEN = 5;
constexpr USIZE SYS_CLOSE = 6;
constexpr USIZE SYS_LSEEK = 19;
constexpr USIZE SYS_OPENAT = 295;

// Device I/O
constexpr USIZE SYS_IOCTL = 54;

// File operations
constexpr USIZE SYS_STAT = 106;
constexpr USIZE SYS_FSTAT = 108;
constexpr USIZE SYS_FSTATAT64 = 300;
constexpr USIZE SYS_UNLINK = 10;

// Directory operations
constexpr USIZE SYS_MKDIR = 39;
constexpr USIZE SYS_RMDIR = 40;
constexpr USIZE SYS_GETDENTS64 = 220;

// Memory operations
constexpr USIZE SYS_MMAP2 = 192;
constexpr USIZE SYS_MUNMAP = 91;

// Socket operations (i386 uses socketcall multiplexer)
constexpr USIZE SYS_SOCKETCALL = 102;
constexpr INT32 SOCKOP_SOCKET = 1;
constexpr INT32 SOCKOP_BIND = 2;
constexpr INT32 SOCKOP_CONNECT = 3;
constexpr INT32 SOCKOP_SEND = 9;
constexpr INT32 SOCKOP_RECV = 10;
constexpr INT32 SOCKOP_SENDTO = 11;
constexpr INT32 SOCKOP_RECVFROM = 12;
constexpr INT32 SOCKOP_SHUTDOWN = 13;
constexpr INT32 SOCKOP_SETSOCKOPT = 14;
constexpr INT32 SOCKOP_GETSOCKOPT = 15;

// Misc
constexpr USIZE SYS_PPOLL = 309;
constexpr USIZE SYS_FCNTL64 = 221;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 265;

// Random operations
constexpr USIZE SYS_GETRANDOM = 355;

// System information
constexpr USIZE SYS_UNAME = 122;

// Process operations
constexpr USIZE SYS_EXIT = 1;
constexpr USIZE SYS_EXIT_GROUP = 252;
constexpr USIZE SYS_FORK = 2;
constexpr USIZE SYS_EXECVE = 11;
constexpr USIZE SYS_DUP2 = 63;
constexpr USIZE SYS_WAIT4 = 114;
constexpr USIZE SYS_KILL = 37;
constexpr USIZE SYS_SETSID = 66;
constexpr USIZE SYS_PIPE = 42;
