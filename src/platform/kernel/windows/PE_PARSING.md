# PE Format Parsing and Export Resolution

[< Back to Windows Kernel README](README.md)

**Files:** [`pe.h`](pe.h), [`pe.cc`](pe.cc)

Runtime PE format parsing to resolve exported function addresses from loaded DLL images. Handles PE32 (32-bit) and PE32+ (64-bit) formats, including forwarded exports with recursive module resolution.

---

## Table of Contents

- [Overview](#overview)
- [PE Header Layout](#pe-header-layout)
- [DOS Header](#dos-header)
- [NT Headers](#nt-headers)
- [Export Directory](#export-directory)
- [Export Resolution Algorithm](#export-resolution-algorithm)
- [Forwarded Exports](#forwarded-exports)
- [32-bit vs 64-bit Differences](#32-bit-vs-64-bit-differences)
- [Usage in the Runtime](#usage-in-the-runtime)

---

## Overview

Every DLL loaded in a Windows process is a PE image mapped into memory. To call a function from a DLL without using import tables, we:

1. Get the DLL's base address (via [PEB walking](PEB_WALKING.md))
2. Parse the PE headers starting from that base address
3. Walk the export directory to find the function by DJB2 hash of its name
4. Return the function's virtual address

This eliminates the need for the Windows loader to populate an Import Address Table (IAT).

---

## PE Header Layout

A PE image in memory has this structure starting from its base address:

```
Base Address (DllBase from PEB)
│
├─ IMAGE_DOS_HEADER          ◄──── offset 0x00
│   ├─ e_magic = 0x5A4D ("MZ")
│   ├─ ... (legacy DOS fields)
│   └─ e_lfanew ─────────────────┐   offset to NT headers
│                                 │
├─ (DOS stub program)             │
│                                 │
├─ IMAGE_NT_HEADERS ◄────────────┘
│   ├─ Signature = 0x00004550 ("PE\0\0")
│   ├─ IMAGE_FILE_HEADER
│   │   ├─ Machine (0x8664=x64, 0xAA64=ARM64, 0x14C=i386, 0x1C4=ARM)
│   │   ├─ NumberOfSections
│   │   └─ SizeOfOptionalHeader
│   └─ IMAGE_OPTIONAL_HEADER (PE32 or PE32+)
│       ├─ Magic (0x10B=PE32, 0x20B=PE32+)
│       ├─ AddressOfEntryPoint
│       ├─ ImageBase
│       └─ DataDirectory[16]
│           ├─ [0] Export Directory ◄──── this is what we need
│           ├─ [1] Import Directory
│           ├─ [2] Resource Directory
│           └─ ...
│
├─ Section Table (.text, .rdata, .data, ...)
│
├─ .text section (code)
├─ .rdata section (read-only data, export tables)
└─ ...
```

---

## DOS Header

The `IMAGE_DOS_HEADER` sits at offset 0 of every PE file. Only two fields matter for PE parsing:

| Field | Offset | Value | Purpose |
|---|---|---|---|
| `e_magic` | 0x00 | `0x5A4D` ("MZ") | Validates this is a PE/DOS image |
| `e_lfanew` | 0x3C | variable | File offset to `IMAGE_NT_HEADERS` |

```c
PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)  // 0x5A4D
    return nullptr;
```

---

## NT Headers

Located at `base + e_lfanew`, the NT headers contain the PE signature, COFF file header, and optional header:

```
IMAGE_NT_HEADERS (64-bit variant)
┌──────────────────────────────────────────────┐
│ Signature        │ 0x00004550 ("PE\0\0")     │
├──────────────────┼──────────────────────────┤
│ FileHeader       │ IMAGE_FILE_HEADER          │
│   Machine        │ Target architecture        │
│   NumberOfSects  │ Number of sections         │
│   Characteristics│ DLL, EXE, etc.             │
├──────────────────┼──────────────────────────┤
│ OptionalHeader   │ IMAGE_OPTIONAL_HEADER64    │
│   Magic          │ 0x20B (PE32+)              │
│   EntryPoint     │ RVA of entry point         │
│   ImageBase      │ Preferred load address     │
│   SizeOfImage    │ Total image size           │
│   DataDirectory  │ [16] entries               │
│     [0].VA       │ Export dir RVA     ◄────── │
│     [0].Size     │ Export dir size    ◄────── │
└──────────────────┴──────────────────────────┘
```

The code uses an architecture alias so the same code works on both 32-bit and 64-bit:

```c
#if defined(PLATFORM_WINDOWS_X86_64) || defined(PLATFORM_WINDOWS_AARCH64)
typedef IMAGE_NT_HEADERS64 IMAGE_NT_HEADERS;   // PE32+
#else
typedef IMAGE_NT_HEADERS32 IMAGE_NT_HEADERS;   // PE32
#endif
```

---

## Export Directory

The export directory (`DataDirectory[0]`) describes all functions a DLL exports. It contains three parallel arrays:

```
IMAGE_EXPORT_DIRECTORY
┌────────────────────────┬────────────────────────────┐
│ Name                   │ RVA → "kernel32.dll"       │
│ Base                   │ Starting ordinal (usually 1)│
│ NumberOfFunctions      │ Total exported functions    │
│ NumberOfNames          │ Named exports count         │
│ AddressOfFunctions     │ RVA → Export Address Table  │
│ AddressOfNames         │ RVA → Name Pointer Table    │
│ AddressOfNameOrdinals  │ RVA → Ordinal Table         │
└────────────────────────┴────────────────────────────┘

The three arrays work together:

  AddressOfNames          AddressOfNameOrdinals     AddressOfFunctions
  (Name Pointer Table)    (Ordinal Table)           (Export Address Table)
  ┌──────────────┐        ┌─────────┐              ┌──────────────┐
  │ RVA→"FuncA"  │──[0]──►│  3      │──[3]────────►│ RVA of FuncX │
  │ RVA→"FuncB"  │──[1]──►│  0      │──[0]────────►│ RVA of FuncA │ ◄── FuncB's code
  │ RVA→"FuncC"  │──[2]──►│  1      │──[1]────────►│ RVA of FuncB │
  └──────────────┘        └─────────┘              │ RVA of FuncC │
                                                    │ RVA of FuncX │
                                                    └──────────────┘
  Names are sorted            Ordinals map           Functions indexed
  alphabetically              name → function        by ordinal
```

**Key insight:** The name table and ordinal table have the same number of entries (`NumberOfNames`). The function table may have more entries (`NumberOfFunctions`) because some functions are exported by ordinal only.

---

## Export Resolution Algorithm

```c
PVOID GetExportAddress(PVOID hModule, UINT64 functionNameHash)
```

Step-by-step:

```
1. Validate DOS header (e_magic == "MZ")
      │ fail → return nullptr
      ▼
2. Locate NT headers (base + e_lfanew)
   Validate NT signature (== "PE\0\0")
      │ fail → return nullptr
      ▼
3. Get export directory RVA from DataDirectory[0]
      │ RVA == 0 → return nullptr (no exports)
      ▼
4. Locate the three arrays:
   nameRvas  = base + ExportDir→AddressOfNames
   funcRvas  = base + ExportDir→AddressOfFunctions
   ordinals  = base + ExportDir→AddressOfNameOrdinals
      ▼
5. For i = 0 to NumberOfNames:
   ┌─────────────────────────────────────────┐
   │ name = base + nameRvas[i]               │
   │ hash = Djb2::Hash(name)                 │
   │                                         │
   │ if (hash == functionNameHash):           │
   │   ordinal = ordinals[i]                 │
   │   funcRva = funcRvas[ordinal]            │
   │                                         │
   │   if funcRva is inside export dir:       │
   │     → forwarded export (see below)       │
   │   else:                                  │
   │     → return (base + funcRva)            │
   └─────────────────────────────────────────┘
      ▼
6. Not found → return nullptr
```

---

## Forwarded Exports

A **forwarded export** occurs when the function RVA points inside the export directory's address range. Instead of pointing to code, it points to an ASCII string like `"NTDLL.RtlInitUnicodeString"`.

```
Export Address Table entry:
  Normal:    RVA → code in .text section
  Forwarded: RVA → "MODULE.FunctionName" string inside export dir
```

The handling code:

```
1. Detect: funcRva >= exportDirRva && funcRva < (exportDirRva + exportDirSize)
      ▼
2. Parse the forward string:
   "NTDLL.RtlInitUnicodeString"
    ─────  ────────────────────
    module      function name
      ▼
3. Build wide module name: L"ntdll.dll"
      ▼
4. Compute DJB2 hash of module name and function name
      ▼
5. GetModuleHandleFromPEB(moduleHash) → target module base
      ▼
6. Recursive: GetExportAddress(targetModule, funcHash)
```

The code builds the wide module name in a stack buffer:

```c
// "NTDLL" → L"ntdll.dll"
WCHAR wideModuleName[64];
for (UINT32 j = 0; j < moduleLen; j++)
    wideModuleName[j] = (WCHAR)forwardStr[j];
wideModuleName[moduleLen]     = L'.';
wideModuleName[moduleLen + 1] = L'd';
wideModuleName[moduleLen + 2] = L'l';
wideModuleName[moduleLen + 3] = L'l';
wideModuleName[moduleLen + 4] = L'\0';
```

> **Safety:** Module name length is capped at 60 characters (`moduleLen + 4 >= 64` check) to prevent stack buffer overflow.

---

## 32-bit vs 64-bit Differences

| Aspect | PE32 (i386/ARM32) | PE32+ (x86_64/ARM64) |
|---|---|---|
| Magic | `0x10B` | `0x20B` |
| `ImageBase` | 4 bytes (`UINT32`) | 8 bytes (`UINT64`) |
| Stack/Heap sizes | 4 bytes each | 8 bytes each |
| `BaseOfData` field | Present | Absent |
| Export directory offset in NT headers | `0x78` | `0x88` |
| Pointer size in structures | 4 bytes | 8 bytes |

The codebase handles this with architecture-conditional typedefs:

```c
#if defined(PLATFORM_WINDOWS_X86_64) || defined(PLATFORM_WINDOWS_AARCH64)
typedef IMAGE_NT_HEADERS64 IMAGE_NT_HEADERS;
#else
typedef IMAGE_NT_HEADERS32 IMAGE_NT_HEADERS;
#endif
```

The raw PE parsing in `system.cc` uses hardcoded offsets that differ per architecture:

```c
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64)
UINT32 exportDirRva  = *(UINT32*)(ntHeaders + 0x88);  // PE32+
#elif defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A)
UINT32 exportDirRva  = *(UINT32*)(ntHeaders + 0x78);  // PE32
#endif
```

---

## Usage in the Runtime

PE export resolution is used in two main contexts:

### 1. Win32 API Resolution

All Win32 wrappers (Kernel32, User32, GDI32) resolve function addresses at call time:

```c
// Inside kernel32.cc — resolves CreateProcessW from kernel32.dll
#define ResolveKernel32ExportAddress(functionName) \
    ResolveExportAddress(L"kernel32.dll", Djb2::HashCompileTime(functionName))

auto fn = (CreateProcessW_t)ResolveKernel32ExportAddress("CreateProcessW");
fn(lpApplicationName, lpCommandLine, ...);
```

### 2. Syscall Resolution

The [indirect syscall system](INDIRECT_SYSCALLS.md) parses ntdll.dll's export table directly (via raw offset math rather than through `GetExportAddress`) for performance:

```c
// In system.cc — direct PE parsing of ntdll exports
UINT8* base = (UINT8*)ntdllBase;
UINT8* ntHeaders = base + *(UINT32*)(base + 0x3C);  // e_lfanew
UINT32 exportDirRva = *(UINT32*)(ntHeaders + 0x88);  // DataDirectory[0]
// ... walk export tables looking for Zw* functions
```

---

[< Back to Windows Kernel README](README.md) | [Previous: PEB Walking](PEB_WALKING.md) | [Next: Indirect Syscalls >](INDIRECT_SYSCALLS.md)
