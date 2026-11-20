/**
 * @file syscall.mips.h
 * @brief MIPS o32 (MIPS32, 32-bit little-endian) Linux syscall numbers.
 *
 * @details Defines the syscall number constants for the MIPS o32 Linux ABI.
 * The o32 table (inherited from IRIX/SVR4, like n64) starts at base 4000 and
 * — like MIPS64 — provides both legacy (open, mkdir, unlink, stat, fork, dup2,
 * pipe) and *at-style variants. This is distinct from the n64 table (base 5000)
 * and from generic arm64/riscv64 (which only provide *at variants).
 *
 * Numbers are the Linux kernel __NR_* values (base 4000), taken from
 * arch/mips/kernel/syscalls/syscall_o32.tbl.
 *
 * @see Linux kernel arch/mips/include/uapi/asm/unistd.h (O32 ABI)
 */
#pragma once

#include "core/types/primitives.h"

// AT_FDCWD (-100) is defined in the common linux/syscall.h
constexpr INT32 AT_REMOVEDIR = 0x200;

// File I/O
constexpr USIZE SYS_READ = 4003;
constexpr USIZE SYS_WRITE = 4004;
constexpr USIZE SYS_OPEN = 4005;
constexpr USIZE SYS_CLOSE = 4006;
constexpr USIZE SYS_LSEEK = 4019;
constexpr USIZE SYS_LSEEK64 = 4140;
constexpr USIZE SYS_OPENAT = 4288;

// Device I/O
constexpr USIZE SYS_IOCTL = 4054;

// File operations
constexpr USIZE SYS_STAT = 4106;
constexpr USIZE SYS_FSTAT = 4108;
constexpr USIZE SYS_FSTATAT = 4293;   // fstatat64 (o32 has no newfstatat; 4293 is fstatat64)
constexpr USIZE SYS_UNLINK = 4010;
constexpr USIZE SYS_UNLINKAT = 4294;

// Directory operations
constexpr USIZE SYS_MKDIR = 4039;
constexpr USIZE SYS_MKDIRAT = 4289;
constexpr USIZE SYS_RMDIR = 4040;
constexpr USIZE SYS_GETDENTS64 = 4219;

// Memory operations
// NOTE: o32 SYS_MMAP (4090) is the legacy single-arg struct-based old_mmap;
// 6-arg mapping uses SYS_MMAP2 (4210, offset in pages). See the mmap wrapper.
constexpr USIZE SYS_MMAP = 4090;
constexpr USIZE SYS_MMAP2 = 4210;
constexpr USIZE SYS_MUNMAP = 4091;

// Socket operations
constexpr USIZE SYS_SOCKET = 4183;
constexpr USIZE SYS_CONNECT = 4170;
constexpr USIZE SYS_SENDTO = 4180;
constexpr USIZE SYS_RECVFROM = 4176;
constexpr USIZE SYS_SHUTDOWN = 4182;
constexpr USIZE SYS_BIND = 4169;
constexpr USIZE SYS_SETSOCKOPT = 4181;
constexpr USIZE SYS_GETSOCKOPT = 4173;
constexpr USIZE SYS_PPOLL = 4302;
constexpr USIZE SYS_FCNTL = 4055;

// Time operations
constexpr USIZE SYS_CLOCK_GETTIME = 4263;

// Random operations
constexpr USIZE SYS_GETRANDOM = 4353;

// System information
constexpr USIZE SYS_UNAME = 4122;

// Process operations
constexpr USIZE SYS_EXIT = 4001;
constexpr USIZE SYS_EXIT_GROUP = 4246;
constexpr USIZE SYS_FORK = 4002;
constexpr USIZE SYS_EXECVE = 4011;
constexpr USIZE SYS_DUP2 = 4063;
constexpr USIZE SYS_WAIT4 = 4114;
constexpr USIZE SYS_KILL = 4037;
constexpr USIZE SYS_SETSID = 4066;
constexpr USIZE SYS_PIPE = 4042;
