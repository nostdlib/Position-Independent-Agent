/**
 * @file syscall.riscv64.h
 * @brief RISC-V 64-bit Linux syscall number definitions.
 *
 * @details Defines the syscall number constants for the RISC-V 64-bit (RV64)
 * Linux kernel ABI. RISC-V uses the generic (asm-generic) syscall table,
 * which is the same as AArch64: exclusively *at-style syscalls (openat,
 * unlinkat, mkdirat) with AT_FDCWD, and modern equivalents (clone instead
 * of fork, dup3 instead of dup2, pipe2 instead of pipe).
 */
#pragma once

#include "core/types/primitives.h"

// riscv64 uses *at syscalls - AT_FDCWD (-100) means use current working dir
constexpr SSIZE AT_FDCWD = -100;
constexpr INT32 AT_REMOVEDIR = 0x200;

// File I/O
constexpr USIZE SYS_READ = 63;
constexpr USIZE SYS_WRITE = 64;
constexpr USIZE SYS_OPENAT = 56;
constexpr USIZE SYS_CLOSE = 57;
constexpr USIZE SYS_LSEEK = 62;

// Device I/O
constexpr USIZE SYS_IOCTL = 29;

// File operations
constexpr USIZE SYS_FSTATAT = 79;
constexpr USIZE SYS_FSTAT = 80;
constexpr USIZE SYS_UNLINKAT = 35;

// Directory operations
constexpr USIZE SYS_MKDIRAT = 34;
constexpr USIZE SYS_GETDENTS64 = 61;

// Memory operations
constexpr USIZE SYS_MMAP = 222;
constexpr USIZE SYS_MUNMAP = 215;

// Socket operations
constexpr USIZE SYS_SOCKET = 198;
constexpr USIZE SYS_BIND = 200;
constexpr USIZE SYS_CONNECT = 203;
constexpr USIZE SYS_SENDTO = 206;
constexpr USIZE SYS_RECVFROM = 207;
constexpr USIZE SYS_SHUTDOWN = 210;
constexpr USIZE SYS_SETSOCKOPT = 208;
constexpr USIZE SYS_GETSOCKOPT = 209;
constexpr USIZE SYS_PPOLL = 73;
constexpr USIZE SYS_FCNTL = 25;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 113;

// Random operations
constexpr USIZE SYS_GETRANDOM = 278;

// System information
constexpr USIZE SYS_UNAME = 160;

// Process operations
constexpr USIZE SYS_EXIT = 93;
constexpr USIZE SYS_EXIT_GROUP = 94;
constexpr USIZE SYS_CLONE = 220;  // riscv64 uses clone instead of fork
constexpr USIZE SYS_EXECVE = 221;
constexpr USIZE SYS_DUP3 = 24;    // riscv64 uses dup3 instead of dup2
constexpr USIZE SYS_WAIT4 = 260;
constexpr USIZE SYS_KILL = 129;
constexpr USIZE SYS_SETSID = 157;
constexpr USIZE SYS_PIPE2 = 59;   // riscv64 uses pipe2 instead of pipe
