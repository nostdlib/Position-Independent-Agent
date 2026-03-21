[< Back to Platform](../README.md) | [< Back to Project Root](../../../README.md)

# Memory Allocation

Platform-independent virtual memory allocation with a hidden size-header trick to support C++ `operator delete` without requiring the caller to supply the allocation size.

## The Size Header Trick (POSIX)

The core challenge: `operator delete(void*)` doesn't provide the allocation size, but `munmap` requires it. The solution is to prepend a `USIZE` header to every allocation:

```
           What mmap returns            What the caller gets
           ┌───────────────────────────┬──────────────────────────────┐
           │ USIZE: totalSize (header) │ usable memory (user data)    │
           └───────────────────────────┴──────────────────────────────┘
           ▲                           ▲
           base (page-aligned)         returned pointer = base + sizeof(USIZE)
```

**Allocation:**
```c
USIZE totalSize = (size + sizeof(USIZE) + 4095) & ~(USIZE)4095;  // page-align
PCHAR base = mmap(NULL, totalSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
*(USIZE*)base = totalSize;           // store size in header
return (PVOID)(base + sizeof(USIZE)); // return pointer past header
```

**Deallocation:**
```c
PCHAR base = (PCHAR)address - sizeof(USIZE);  // recover base
USIZE totalSize = *(USIZE*)base;               // read stored size
munmap(base, totalSize);                        // free entire mapping
```

This means every allocation is at least one full page (4096 bytes) — wasteful for small allocations, but acceptable since the runtime makes relatively few allocations, and the simplicity avoids needing a full heap allocator.

## mmap2 Page-Shift (32-bit Linux)

32-bit Linux architectures (i386, ARMv7-A, RISC-V 32) use `SYS_MMAP2` instead of `SYS_MMAP`. The difference: the offset parameter is in **pages** (offset / 4096), not bytes. This extends the addressable file offset range on 32-bit systems. For anonymous mappings (offset = 0), the difference is invisible.

## FreeBSD i386 Inline Assembly Hack

FreeBSD's `mmap` takes an `off_t` offset parameter, which is a **64-bit** type even on i386. This means 7 argument slots on the stack (the 64-bit offset occupies two 32-bit slots). The 6-argument `System::Call` can't handle this — it only pushes one slot for the offset, leaving garbage in the upper 32 bits.

The fix is raw inline assembly that manually pushes all 8 stack slots (7 args + dummy return address):

```asm
pushl $0          ; off_t high 32 bits = 0
pushl $0          ; off_t low 32 bits = 0
pushl %%edi       ; fd = -1
pushl %%esi       ; flags = MAP_PRIVATE | MAP_ANONYMOUS
pushl %%edx       ; prot = PROT_READ | PROT_WRITE
pushl %%ecx       ; len = totalSize
pushl %%ebx       ; addr = NULL
pushl $0          ; dummy return address (FreeBSD expects this)
int $0x80         ; BSD syscall
jnc 1f            ; carry flag clear = success
negl %%eax        ; carry set = negate errno
1:
addl $32, %%esp   ; clean up 32 bytes (8 × 4)
```

Without this fix, `mmap` returns `EINVAL` because FreeBSD rejects non-zero offsets for `MAP_ANONYMOUS` — and the garbage upper bits make the offset appear non-zero.

## Windows: No Header Needed

`ZwAllocateVirtualMemory` with `MEM_COMMIT | MEM_RESERVE` allocates pages, and `ZwFreeVirtualMemory` with `MEM_RELEASE` frees the entire region by base address alone — no size parameter needed for full-region release. So no header trick is required.

## UEFI: No Header Needed

`BootServices→AllocatePool(EfiLoaderData, size, &buffer)` and `BootServices→FreePool(buffer)` — the UEFI pool allocator tracks sizes internally.

## Global Operator Overloads

All C++ heap allocations are intercepted so the runtime never calls `malloc`/`free`:

```c++
void* operator new(size_t size)            { return Allocator::AllocateMemory(size); }
void* operator new[](size_t size)          { return Allocator::AllocateMemory(size); }
void  operator delete(void* p) noexcept    { Allocator::ReleaseMemory(p, 0); }
void  operator delete(void* p, size_t s) noexcept { Allocator::ReleaseMemory(p, s); }
```
