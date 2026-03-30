/**
 * @file memory.cc
 * @brief Windows memory allocation implementation.
 * @details Provides Allocator::AllocateMemory and ReleaseMemory using
 * NtAllocateVirtualMemory/NtFreeVirtualMemory via NTDLL wrappers.
 */

#include "platform/memory/allocator.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/kernel/windows/windows_types.h"

// Memory allocator using ZwAllocateVirtualMemory 
PVOID Allocator::AllocateMemory(USIZE len)
{
	if (len == 0)
		return nullptr;

	PVOID base = nullptr;
	USIZE size = len;
	auto result = NTDLL::ZwAllocateVirtualMemory(NTDLL::NtCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	return result ? base : nullptr;
}

// Release memory allocated by AllocateMemory
VOID Allocator::ReleaseMemory(PVOID ptr, USIZE)
{
	if (ptr == nullptr)
		return;

	USIZE size = 0;
	(VOID)NTDLL::ZwFreeVirtualMemory(NTDLL::NtCurrentProcess(), &ptr, &size, MEM_RELEASE);
}