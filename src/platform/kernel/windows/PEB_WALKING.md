# PEB Walking and Module Resolution

[< Back to Windows Kernel README](README.md)

**Files:** [`peb.h`](peb.h), [`peb.cc`](peb.cc)

The Process Environment Block (PEB) is a user-mode structure maintained by the NT loader containing process startup information, loaded module lists, and heap/environment pointers. This subsystem provides position-independent PEB access for dynamic module discovery — no Win32 API imports, no static import tables.

---

## Table of Contents

- [Overview](#overview)
- [TEB and PEB Access](#teb-and-peb-access)
- [PEB Structure Layout](#peb-structure-layout)
- [Loader Data and Module Lists](#loader-data-and-module-lists)
- [LDR_DATA_TABLE_ENTRY](#ldr_data_table_entry)
- [Module Resolution by DJB2 Hash](#module-resolution-by-djb2-hash)
- [Export Resolution via PEB](#export-resolution-via-peb)
- [Complete Resolution Flow](#complete-resolution-flow)
- [Architecture-Specific Details](#architecture-specific-details)

---

## Overview

On Windows, every process has a PEB allocated by the kernel at process creation. The PEB is accessible from user mode through the Thread Environment Block (TEB), which is pointed to by a dedicated CPU register on each architecture. The PEB contains:

- **LoaderData** — linked lists of all loaded modules (DLLs and the EXE)
- **ProcessParameters** — command line, environment, standard I/O handles
- **ImageBase** — base address of the process executable
- **ProcessHeap** — handle to the default process heap

By walking the PEB's module lists, we can locate any loaded DLL's base address at runtime without calling `GetModuleHandle` or `LoadLibrary`.

---

## TEB and PEB Access

The TEB (Thread Environment Block) is always accessible via a dedicated segment/register. The PEB pointer lives at a fixed offset within the TEB:

| Architecture | TEB Register | PEB Offset | Inline Assembly |
|---|---|---|---|
| **x86_64** | `GS` segment | `0x60` | `mov rax, gs:[0x60]` |
| **i386** | `FS` segment | `0x30` | `mov eax, fs:[0x30]` |
| **ARM64** | `X18` register | `0x60` | `ldr x0, [x18, #0x60]` |
| **ARM32** | `R9` register | `0x30` | `ldr r0, [r9, #0x30]` |

Implementation in `peb.cc`:

```c
PPEB GetCurrentPEB(VOID)
{
    PPEB peb;
#if defined(PLATFORM_WINDOWS_X86_64)
    __asm__("movq %%gs:%1, %0" : "=r"(peb) : "m"(*(PUINT64)(0x60)));
#elif defined(PLATFORM_WINDOWS_I386)
    __asm__("movl %%fs:%1, %0" : "=r"(peb) : "m"(*(PUINT32)(0x30)));
#elif defined(PLATFORM_WINDOWS_ARMV7A)
    __asm__("ldr %0, [r9, %1]" : "=r"(peb) : "i"(0x30));
#elif defined(PLATFORM_WINDOWS_AARCH64)
    __asm__("ldr %0, [x18, #%1]" : "=r"(peb) : "i"(0x60));
#endif
    return peb;
}
```

---

## PEB Structure Layout

Our minimal PEB definition contains only the fields needed by the runtime:

```
┌─────────────────────────────────────────────────────┐
│ PEB                                                 │
├──────────────────────┬──────────────────────────────┤
│ InheritedAddressSpace│ UINT8   (offset 0x00)        │
│ ReadImageFileExecOpts│ UINT8   (offset 0x01)        │
│ BeingDebugged        │ UINT8   (offset 0x02)  ◄──── can detect debugger
│ Spare                │ UINT8   (offset 0x03)        │
│ Mutant               │ PVOID   (offset 0x04/0x08)   │
│ ImageBase            │ PVOID                  ◄──── process EXE base address
│ LoaderData           │ PEB_LDR_DATA*          ◄──── pointer to module lists
│ ProcessParameters    │ RTL_USER_PROCESS_PARAMS*◄──── stdin/stdout/stderr handles
│ SubSystemData        │ PVOID                        │
│ ProcessHeap          │ PVOID                  ◄──── default heap handle
└──────────────────────┴──────────────────────────────┘
```

> **Note:** `BeingDebugged` is set to `TRUE` when a debugger is attached. This field is commonly checked for anti-debugging, though our code does not use it for that purpose.

---

## Loader Data and Module Lists

`PEB→LoaderData` points to a `PEB_LDR_DATA` structure containing three circular doubly-linked list heads:

```
┌────────────────────────────────────────────┐
│ PEB_LDR_DATA                               │
├────────────────────────────────────────────┤
│ Length              │ UINT32                │
│ Initialized         │ UINT32                │
│ SsHandle            │ PVOID                 │
│ InLoadOrderModuleList         ◄──── load order (order DLLs were loaded)
│ InMemoryOrderModuleList       ◄──── memory order (sorted by base address) ★ WE USE THIS
│ InInitializationOrderModuleList◄──── init order (order DllMain was called)
└────────────────────────────────────────────┘
```

Each list head is a `LIST_ENTRY` — a circular doubly-linked list node:

```
          ┌──────────┐     ┌──────────┐     ┌──────────┐
  ┌──────►│ List Head │────►│ Module 1 │────►│ Module 2 │────┐
  │       │  (PEB_   │◄────│ (ntdll)  │◄────│(kernel32)│    │
  │       │ LDR_DATA)│     └──────────┘     └──────────┘    │
  │       └──────────┘◄──────────────────────────────────────┘
  └──────────────────────────────────────────────────────────┘
               Circular doubly-linked list (Flink/Blink)
```

**Why InMemoryOrderModuleList?** — This list is commonly used because `ntdll.dll` and `kernel32.dll` are typically the first two entries (they are always loaded and mapped at low addresses), making lookups predictable.

---

## LDR_DATA_TABLE_ENTRY

Each loaded module is described by an `LDR_DATA_TABLE_ENTRY`:

```
┌─────────────────────────────────────────────────────┐
│ LDR_DATA_TABLE_ENTRY                                │
├──────────────────────────┬──────────────────────────┤
│ InLoadOrderModuleList    │ LIST_ENTRY               │
│ InMemoryOrderModuleList  │ LIST_ENTRY         ◄──── we traverse via this
│ InInitializationOrderList│ LIST_ENTRY               │
│ DllBase                  │ PVOID              ◄──── module base address (what we want)
│ EntryPoint               │ PVOID              ◄──── DllMain address
│ SizeOfImage              │ UINT32                   │
│ FullDllName              │ UNICODE_STRING     ◄──── e.g., "C:\Windows\System32\ntdll.dll"
│ BaseDllName              │ UNICODE_STRING     ◄──── e.g., "ntdll.dll" (we hash this)
│ Flags                    │ UINT32                   │
│ LoadCount                │ INT16                    │
│ TlsIndex                 │ INT16                    │
│ HashTableEntry           │ LIST_ENTRY               │
│ TimeDateStamp            │ UINT32                   │
└──────────────────────────┴──────────────────────────┘
```

The `CONTAINING_RECORD` macro is essential for navigating from a `LIST_ENTRY` pointer back to the containing `LDR_DATA_TABLE_ENTRY`:

```c
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((PCHAR)(address) - (USIZE)(&((type *)0)->field)))
```

When walking `InMemoryOrderModuleList`, each `Flink` points to the `InMemoryOrderModuleList` field of the next entry — not to the start of the structure. `CONTAINING_RECORD` subtracts the field offset to get the actual `LDR_DATA_TABLE_ENTRY` base.

---

## Module Resolution by DJB2 Hash

Instead of comparing module names as strings (which would embed plaintext DLL names in the binary), we compare DJB2 hashes:

```c
PVOID GetModuleHandleFromPEB(UINT64 moduleNameHash)
{
    PPEB peb = GetCurrentPEB();
    PLIST_ENTRY list = &peb->LoaderData->InMemoryOrderModuleList;
    PLIST_ENTRY entry = list->Flink;

    while (entry != list)  // circular list — stop when we reach the head again
    {
        PLDR_DATA_TABLE_ENTRY module =
            CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderModuleList);

        if (module->BaseDllName.Buffer != nullptr &&
            Djb2::Hash(module->BaseDllName.Buffer) == moduleNameHash)
            return module->DllBase;

        entry = entry->Flink;
    }

    return nullptr;
}
```

**Key points:**
- The DJB2 hash of the module name is computed at compile time via `Djb2::HashCompileTime(L"ntdll.dll")`, so no string appears in the binary
- `BaseDllName` is a `UNICODE_STRING` (UTF-16LE) — the hash function handles wide characters
- Hash comparison is case-insensitive (the hash function lowercases before hashing)
- The walk terminates when `entry == list` (we've gone full circle)

---

## Export Resolution via PEB

Two higher-level functions build on `GetModuleHandleFromPEB`:

### `ResolveExportAddressFromPebModule(moduleHash, funcHash)`

Direct two-step resolution for modules known to be loaded:

1. `GetModuleHandleFromPEB(moduleHash)` → module base
2. `GetExportAddress(moduleBase, funcHash)` → function pointer

### `ResolveExportAddress(moduleName, funcHash)`

Resolution with automatic loading for modules that might not be in memory yet:

1. **Fast path:** compute DJB2 hash of `moduleName`, try `GetModuleHandleFromPEB`
2. **Slow path:** if module not found, load it via `NTDLL::LdrLoadDll`, then resolve the export

```c
PVOID ResolveExportAddress(const WCHAR *moduleName, UINT64 functionNameHash)
{
    UINT64 moduleNameHash = Djb2::Hash(moduleName);

    // Fast path: already loaded
    PVOID moduleBase = GetModuleHandleFromPEB(moduleNameHash);

    // Slow path: load via LdrLoadDll
    if (moduleBase == nullptr)
    {
        UNICODE_STRING dllName;
        dllName.Length = nameLen * sizeof(WCHAR);
        dllName.MaximumLength = dllName.Length + sizeof(WCHAR);
        dllName.Buffer = (PWCHAR)moduleName;

        NTDLL::LdrLoadDll(nullptr, nullptr, &dllName, &moduleBase);
    }

    return GetExportAddress(moduleBase, functionNameHash);
}
```

---

## Complete Resolution Flow

```
ResolveExportAddress(L"kernel32.dll", Djb2::HashCompileTime("CreateProcessW"))
  │
  ├─ Djb2::Hash(L"kernel32.dll") → 0x6A4ABC5B (example)
  │
  ├─ GetModuleHandleFromPEB(0x6A4ABC5B)
  │    │
  │    ├─ GetCurrentPEB()
  │    │    └─ x86_64: mov rax, gs:[0x60] → PEB*
  │    │
  │    ├─ PEB→LoaderData→InMemoryOrderModuleList (list head)
  │    │
  │    ├─ entry = head→Flink (first module)
  │    │    ├─ CONTAINING_RECORD → LDR_DATA_TABLE_ENTRY
  │    │    ├─ Djb2::Hash("ntdll.dll") → 0x1EDAB0ED (no match)
  │    │    └─ entry = entry→Flink
  │    │
  │    ├─ entry (second module)
  │    │    ├─ CONTAINING_RECORD → LDR_DATA_TABLE_ENTRY
  │    │    ├─ Djb2::Hash("KERNEL32.DLL") → 0x6A4ABC5B (match!)
  │    │    └─ return DllBase = 0x00007FF8A1200000
  │    │
  │    └─ (would continue if no match until entry == head)
  │
  └─ GetExportAddress(0x00007FF8A1200000, funcHash)
       └─ [see PE_PARSING.md for details]
```

---

## Architecture-Specific Details

### x86_64 (64-bit Intel)

- TEB is pointed to by the `GS` segment base (set by the kernel via `MSR_GS_BASE`)
- PEB is at `GS:[0x60]` (offset 0x60 in the TEB)
- Pointers in `PEB`, `PEB_LDR_DATA`, and `LDR_DATA_TABLE_ENTRY` are 8 bytes

### i386 (32-bit Intel)

- TEB is pointed to by the `FS` segment base (set via the GDT)
- PEB is at `FS:[0x30]` (offset 0x30 in the TEB)
- Pointers are 4 bytes — structure layouts are smaller

### ARM64 (AArch64)

- TEB pointer is stored in the `X18` platform register (reserved by Windows ABI)
- PEB is at `[X18, #0x60]`
- Same pointer sizes as x86_64 (8 bytes)

### ARM32 (ARMv7-A)

- TEB pointer is stored in the `R9` register (reserved by Windows ARM ABI)
- PEB is at `[R9, #0x30]`
- Same pointer sizes as i386 (4 bytes)

---

[< Back to Windows Kernel README](README.md) | [Next: PE Parsing >](PE_PARSING.md)
