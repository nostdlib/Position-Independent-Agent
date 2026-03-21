/**
 * @file syscall.mips64.h
 * @brief MIPS64 Linux syscall number definitions (n64 ABI).
 *
 * @details Defines the syscall number constants for the MIPS64 Linux kernel
 * n64 ABI. MIPS64 uses its own syscall table inherited from IRIX/SVR4, with
 * numbers starting at 5000. Unlike aarch64/riscv64 (which only provide
 * *at-style syscalls), MIPS64 provides both legacy (open, mkdir, unlink,
 * stat, fork, dup2, pipe) and *at-style variants.
 *
 * @see Linux kernel arch/mips/include/uapi/asm/unistd.h (N64 ABI)
 */
#pragma once

#include "core/types/primitives.h"

// *at-style file descriptor base
constexpr SSIZE AT_FDCWD = -100;
constexpr INT32 AT_REMOVEDIR = 0x200;

// File I/O
constexpr USIZE SYS_READ = 5000;
constexpr USIZE SYS_WRITE = 5001;
constexpr USIZE SYS_OPEN = 5002;
constexpr USIZE SYS_CLOSE = 5003;
constexpr USIZE SYS_LSEEK = 5008;
constexpr USIZE SYS_OPENAT = 5247;

// Device I/O
constexpr USIZE SYS_IOCTL = 5015;

// File operations
constexpr USIZE SYS_STAT = 5004;
constexpr USIZE SYS_FSTAT = 5005;
constexpr USIZE SYS_FSTATAT = 5256;
constexpr USIZE SYS_UNLINK = 5087;
constexpr USIZE SYS_UNLINKAT = 5233;

// Directory operations
constexpr USIZE SYS_MKDIR = 5083;
constexpr USIZE SYS_MKDIRAT = 5252;
constexpr USIZE SYS_RMDIR = 5084;
constexpr USIZE SYS_GETDENTS64 = 5308;

// Memory operations
constexpr USIZE SYS_MMAP = 5009;
constexpr USIZE SYS_MUNMAP = 5011;

// Socket operations
constexpr USIZE SYS_SOCKET = 5041;
constexpr USIZE SYS_CONNECT = 5042;
constexpr USIZE SYS_SENDTO = 5044;
constexpr USIZE SYS_RECVFROM = 5045;
constexpr USIZE SYS_SHUTDOWN = 5048;
constexpr USIZE SYS_BIND = 5049;
constexpr USIZE SYS_SETSOCKOPT = 5054;
constexpr USIZE SYS_GETSOCKOPT = 5055;
constexpr USIZE SYS_PPOLL = 5272;
constexpr USIZE SYS_FCNTL = 5070;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 5222;

// Random operations
constexpr USIZE SYS_GETRANDOM = 5313;

// System information
constexpr USIZE SYS_UNAME = 5061;

// Process operations
constexpr USIZE SYS_EXIT = 5058;
constexpr USIZE SYS_EXIT_GROUP = 5205;
constexpr USIZE SYS_FORK = 5056;
constexpr USIZE SYS_EXECVE = 5057;
constexpr USIZE SYS_DUP2 = 5033;
constexpr USIZE SYS_WAIT4 = 5059;
constexpr USIZE SYS_KILL = 5037;
constexpr USIZE SYS_SETSID = 5112;
constexpr USIZE SYS_PIPE = 5021;
