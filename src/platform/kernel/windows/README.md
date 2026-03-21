[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# Windows NT Kernel Interface

Position-independent Windows NT kernel layer: direct syscall dispatch, PEB-based module resolution, PE export parsing, and Win32 API wrappers — no Windows SDK, no CRT, no static import tables.

## Architecture Support

| Architecture | Register for PEB | Syscall Mechanism | Calling Convention |
|---|---|---|---|
| **x86_64** | `GS:[0x60]` | Indirect `syscall` gadget (`0F 05 C3`) in ntdll | `R10/RDX/R8/R9` + stack shadow space |
| **i386** | `FS:[0x30]` | `call [edx]` or `call edx` via ntdll stub | All args pushed on stack, `EAX` = SSN |
| **ARM64** | `X18 + 0x60` | `BLR` to ntdll `SVC #N; RET` stub | `X0-X7` + stack |
| **ARM32** | `R9 + 0x30` | `BLX` to ntdll `SVC #1` stub (Thumb-2) | `R0-R3` + stack |

## Deep Dives

- [PEB Walking & Module Resolution](PEB_WALKING.md) — TEB register access per architecture, `InMemoryOrderModuleList` traversal via `CONTAINING_RECORD`, DJB2 hash matching, fast/slow path with `LdrLoadDll` fallback
- [PE Format Parsing](PE_PARSING.md) — DOS/NT headers, export directory three-array system, name-to-ordinal-to-RVA resolution, forwarded export handling with recursive module resolution, PE32 vs PE32+ offset differences
- [Indirect Syscall Dispatch](INDIRECT_SYSCALLS.md) — Runtime SSN resolution by counting `Zw*` exports, ntdll stub scanning for `syscall;ret` gadget (`0F 05 C3`), i386 old/new stub formats, ARM64 `SVC+RET` pair discovery, `System::Call` overloads for 0-14 arguments
- [NT Native API Wrappers](NTDLL_WRAPPERS.md) — 23 `Zw*` syscall wrappers with dual-path dispatch (indirect syscall primary, direct call fallback), 5 `Rtl*` functions, `LdrLoadDll`, NTSTATUS error codes
- [Win32 API Wrappers](WIN32_WRAPPERS.md) — Kernel32 process creation with pipe plumbing, User32 display enumeration, GDI32 screen capture pipeline (`BitBlt` → `GetDIBits`), dynamic resolution via DJB2 hash

## How It All Connects

```
                    ┌─────────────────────────────────┐
                    │        Win32 Wrappers            │
                    │  Kernel32 · User32 · GDI32       │
                    └───────────────┬──────────────────┘
                                    │ ResolveExportAddress()
                    ┌───────────────▼──────────────────┐
                    │      NT Native API (NTDLL)       │
                    │   ZwCreateFile · ZwReadFile ...   │
                    └──────┬────────────────┬──────────┘
                           │                │
              ResolveSyscall()      CALL_FUNCTION()
              (indirect path)       (direct fallback)
                           │                │
                    ┌──────▼───────┐  ┌─────▼──────────┐
                    │   System     │  │ PEB Export      │
                    │   ::Call()   │  │ Resolution      │
                    │  (inline     │  │ (ResolveExport  │
                    │   assembly)  │  │  Address)       │
                    └──────┬───────┘  └─────┬──────────┘
                           │                │
                    ┌──────▼───────┐  ┌─────▼──────────┐
                    │  Syscall     │  │  PEB Walking    │
                    │  Gadget in   │  │  GetModuleHandle│
                    │  ntdll.dll   │  │  FromPEB()      │
                    └──────┬───────┘  └─────┬──────────┘
                           │                │
                    ┌──────▼───────┐  ┌─────▼──────────┐
                    │  PE Export   │  │  PE Export      │
                    │  Parsing     │  │  Parsing        │
                    │  (system.cc) │  │  (pe.cc)        │
                    └──────────────┘  └────────────────┘
```

## Quick Reference

### Wrapped Syscalls (23 functions)

| Category | Functions |
|---|---|
| **File I/O** | `ZwCreateFile`, `ZwOpenFile`, `ZwReadFile`, `ZwWriteFile`, `ZwClose`, `ZwDeleteFile` |
| **File Metadata** | `ZwQueryInformationFile`, `ZwSetInformationFile`, `ZwQueryAttributesFile`, `ZwQueryDirectoryFile`, `ZwQueryVolumeInformationFile` |
| **Memory** | `ZwAllocateVirtualMemory`, `ZwFreeVirtualMemory`, `ZwProtectVirtualMemory` |
| **Process** | `ZwTerminateProcess`, `ZwCreateUserProcess`, `ZwQueryInformationProcess` |
| **Sync** | `ZwCreateEvent`, `ZwWaitForSingleObject` |
| **Device/Pipe** | `ZwDeviceIoControlFile`, `ZwCreateNamedPipeFile`, `ZwSetInformationObject` |
| **System** | `ZwQuerySystemInformation` |

### Win32 Wrappers (15 functions)

| DLL | Functions |
|---|---|
| **kernel32** | `CreateProcessW`, `SetHandleInformation`, `CreatePipe`, `PeekNamedPipe` |
| **user32** | `EnumDisplayDevicesW`, `EnumDisplaySettingsW`, `GetDC`, `ReleaseDC` |
| **gdi32** | `CreateCompatibleDC`, `CreateCompatibleBitmap`, `SelectObject`, `BitBlt`, `GetDIBits`, `DeleteDC`, `DeleteObject` |

### Windows Types (`windows_types.h`)

| Category | Types / Constants |
|---|---|
| **NT Objects** | `UNICODE_STRING`, `OBJECT_ATTRIBUTES`, `IO_STATUS_BLOCK`, `LARGE_INTEGER` |
| **File** | `FILE_OPEN`, `FILE_CREATE`, `FILE_SYNCHRONOUS_IO_NONALERT`, `FILE_ATTRIBUTE_*` |
| **Access** | `GENERIC_READ`, `GENERIC_WRITE`, `SYNCHRONIZE`, `DELETE` |
| **Memory** | `MEM_COMMIT`, `MEM_RESERVE`, `MEM_RELEASE`, `PAGE_READWRITE` |
| **Firmware** | `SYSTEM_FIRMWARE_TABLE_INFORMATION`, `RAW_SMBIOS_DATA`, `SMBIOS_HEADER` |
| **Helpers** | `InitializeObjectAttributes`, `INVALID_HANDLE_VALUE` |

### NTSTATUS Error Handling (`platform_result.h`)

`result::FromNTSTATUS<T>(status)` — converts `NTSTATUS` to `Result<T, Error>` using NT_SUCCESS semantics (`status >= 0`). Failed statuses are wrapped in `Error::Windows(status)`.

## Design Principles

1. **Zero Dependencies** — No Windows SDK, no CRT, no static import tables. All types are custom-defined.
2. **Position-Independent** — All module and function resolution happens at runtime via PEB walking and DJB2 hashing. No IAT entries exist.
3. **No Data Sections** — String literals and constants are compiled into the `.text` section via a custom LLVM pass, ensuring the entire binary operates from read-execute memory.
4. **Indirect Syscalls** — System calls go through gadgets found within ntdll.dll rather than calling ntdll exports directly, making the calls originate from ntdll's address space.
5. **Multi-Architecture** — Identical API surface across x86_64, i386, ARM64, and ARM32 with architecture-specific inline assembly isolated in per-arch headers.
