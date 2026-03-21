/**
 * @file syscall.x86_64.h
 * @brief x86_64 Linux syscall number definitions.
 *
 * @details Defines the syscall number constants for the x86_64 Linux kernel ABI.
 * Covers file I/O, file operations, directory operations, memory management,
 * socket operations, time, random, and process control syscalls.
 */
#pragma once

#include "core/types/primitives.h"

// File I/O
constexpr USIZE SYS_READ = 0;
constexpr USIZE SYS_WRITE = 1;
constexpr USIZE SYS_OPEN = 2;
constexpr USIZE SYS_CLOSE = 3;
constexpr USIZE SYS_LSEEK = 8;
constexpr USIZE SYS_OPENAT = 257;

// Device I/O
constexpr USIZE SYS_IOCTL = 16;

// File operations
constexpr USIZE SYS_STAT = 4;
constexpr USIZE SYS_FSTAT = 5;
constexpr USIZE SYS_NEWFSTATAT = 262;
constexpr USIZE SYS_UNLINK = 87;

// Directory operations
constexpr USIZE SYS_MKDIR = 83;
constexpr USIZE SYS_RMDIR = 84;
constexpr USIZE SYS_GETDENTS64 = 217;

// Memory operations
constexpr USIZE SYS_MMAP = 9;
constexpr USIZE SYS_MUNMAP = 11;

// Socket operations
constexpr USIZE SYS_SOCKET = 41;
constexpr USIZE SYS_CONNECT = 42;
constexpr USIZE SYS_SENDTO = 44;
constexpr USIZE SYS_RECVFROM = 45;
constexpr USIZE SYS_SHUTDOWN = 48;
constexpr USIZE SYS_BIND = 49;
constexpr USIZE SYS_SETSOCKOPT = 54;
constexpr USIZE SYS_GETSOCKOPT = 55;
constexpr USIZE SYS_PPOLL = 271;
constexpr USIZE SYS_FCNTL = 72;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 228;

// Random operations
constexpr USIZE SYS_GETRANDOM = 318;

// System information
constexpr USIZE SYS_UNAME = 63;

// Process operations
constexpr USIZE SYS_EXIT = 60;
constexpr USIZE SYS_EXIT_GROUP = 231;
constexpr USIZE SYS_FORK = 57;
constexpr USIZE SYS_EXECVE = 59;
constexpr USIZE SYS_DUP2 = 33;
constexpr USIZE SYS_WAIT4 = 61;
constexpr USIZE SYS_KILL = 62;
constexpr USIZE SYS_SETSID = 112;
constexpr USIZE SYS_PIPE = 22;
