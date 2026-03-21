# NT Native API Wrappers (ntdll.dll)

[< Back to Windows Kernel README](README.md)

**Files:** [`ntdll.h`](ntdll.h), [`ntdll.cc`](ntdll.cc)

The `NTDLL` class wraps NT Native API functions exported by ntdll.dll — the lowest-level user-mode API, consisting of thin stubs that transition into the kernel via syscalls. All Win32 APIs (kernel32, user32, etc.) are built on top of these.

---

## Table of Contents

- [Dual-Path Dispatch Pattern](#dual-path-dispatch-pattern)
- [File Operations](#file-operations)
- [Memory Operations](#memory-operations)
- [Process and Thread Operations](#process-and-thread-operations)
- [Synchronization](#synchronization)
- [Device I/O](#device-io)
- [Pipe Operations](#pipe-operations)
- [Object Management](#object-management)
- [System Information](#system-information)
- [Runtime Library Functions (Rtl*)](#runtime-library-functions-rtl)
- [Module Loading](#module-loading)
- [Error Handling](#error-handling)

---

## Dual-Path Dispatch Pattern

Every `Zw*` wrapper follows the same pattern — try indirect syscall first, fall back to direct function call:

```c
Result<NTSTATUS, Error> NTDLL::ZwCreateFile(/* params */)
{
    // 1. Resolve the SSN and syscall gadget at runtime
    SYSCALL_ENTRY entry = ResolveSyscall("ZwCreateFile");

    // 2. Dispatch via indirect syscall (preferred) or direct call (fallback)
    NTSTATUS status = entry.Ssn != SYSCALL_SSN_INVALID
        ? System::Call(entry, (USIZE)arg1, (USIZE)arg2, ...)   // indirect syscall
        : CALL_FUNCTION("ZwCreateFile", ...);                    // direct ntdll call

    // 3. Convert NTSTATUS to Result<T, Error>
    return result::FromNTSTATUS<NTSTATUS>(status);
}
```

**Why two paths?**
- The indirect syscall path is the primary mechanism on x86_64 and i386
- The `CALL_FUNCTION` fallback exists for architectures where indirect syscalls are not yet fully implemented (ARM64/ARM32 — marked as TODO)
- Currently `CALL_FUNCTION` returns `-1` as a placeholder

---

## File Operations

### ZwCreateFile

Opens or creates a file, directory, device, or named pipe.

```
ZwCreateFile(
    FileHandle,          // OUT: receives the file handle
    DesiredAccess,       // GENERIC_READ, GENERIC_WRITE, SYNCHRONIZE, etc.
    ObjectAttributes,    // NT path, attributes, root directory
    IoStatusBlock,       // OUT: status and bytes transferred
    AllocationSize,      // initial allocation size (or NULL)
    FileAttributes,      // FILE_ATTRIBUTE_NORMAL, etc.
    ShareAccess,         // FILE_SHARE_READ | FILE_SHARE_WRITE
    CreateDisposition,   // FILE_OPEN, FILE_CREATE, FILE_OVERWRITE_IF
    CreateOptions,       // FILE_SYNCHRONOUS_IO_NONALERT, FILE_NON_DIRECTORY_FILE
    EaBuffer,            // extended attributes (usually NULL)
    EaLength             // length of EA buffer
)
```

**11 arguments** — the most complex file operation.

### ZwOpenFile

Opens an existing file (simplified ZwCreateFile with fewer parameters).

```
ZwOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions)
```

**6 arguments.**

### ZwReadFile / ZwWriteFile

Read or write data to/from a file handle. Both take **9 arguments**:

```
ZwReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
           Buffer, Length, ByteOffset, Key)
```

- `Event` — optional event to signal on completion (NULL for synchronous)
- `ApcRoutine`/`ApcContext` — asynchronous procedure call (usually NULL)
- `ByteOffset` — file position (or NULL to use current position)
- `Key` — file lock key (usually NULL)

### ZwQueryInformationFile / ZwSetInformationFile

Query or modify file metadata. **5 arguments** each:

```
ZwQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length,
                       FileInformationClass)
```

Common `FileInformationClass` values:
- `FileBasicInformation` (4) — timestamps and attributes
- `FileStandardInformation` (5) — size, link count, delete pending
- `FilePositionInformation` (14) — current file pointer
- `FileDispositionInformation` (13) — mark for deletion

### ZwQueryAttributesFile

Quick query of basic file attributes without opening the file. **2 arguments.**

### ZwDeleteFile

Delete a file by its OBJECT_ATTRIBUTES (NT path). **1 argument.**

### ZwQueryDirectoryFile

Enumerate directory contents. **11 arguments:**

```
ZwQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                     FileInformation, Length, FileInformationClass,
                     ReturnSingleEntry, FileName, RestartScan)
```

- `FileInformationClass` — typically `FileBothDirectoryInformation` (3)
- `FileName` — optional wildcard filter (e.g., L"*.txt")
- `RestartScan` — TRUE to start from the beginning

### ZwQueryVolumeInformationFile

Query volume/device information for a file handle. **5 arguments.** Used to determine the device type (disk, network, CD-ROM, etc.).

---

## Memory Operations

### ZwAllocateVirtualMemory

Allocate virtual memory pages in a process address space. **6 arguments:**

```
ZwAllocateVirtualMemory(
    ProcessHandle,    // NtCurrentProcess() = (PVOID)-1
    BaseAddress,      // IN/OUT: requested/actual base address
    ZeroBits,         // address space constraint (usually 0)
    RegionSize,       // IN/OUT: requested/actual size
    AllocationType,   // MEM_COMMIT | MEM_RESERVE
    Protect           // PAGE_READWRITE, PAGE_EXECUTE_READ, etc.
)
```

### ZwFreeVirtualMemory

Free previously allocated virtual memory. **4 arguments:**

```
ZwFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType)
```

- `FreeType` — `MEM_RELEASE` (free entire region) or `MEM_DECOMMIT` (decommit pages)

### ZwProtectVirtualMemory

Change the protection of virtual memory pages. **5 arguments.** Used to transition memory between RW (for writing) and RX (for execution).

---

## Process and Thread Operations

### ZwTerminateProcess

Terminate a process. **2 arguments:**

```
ZwTerminateProcess(ProcessHandle, ExitStatus)
```

- `ProcessHandle` — target process, or `NtCurrentProcess()` for self
- `ExitStatus` — the exit code

### ZwCreateUserProcess

Create a new user-mode process (NT6+ replacement for the older NtCreateProcess). **11 arguments:**

```
ZwCreateUserProcess(
    ProcessHandle, ThreadHandle,
    ProcessDesiredAccess, ThreadDesiredAccess,
    ProcessObjectAttributes, ThreadObjectAttributes,
    ProcessFlags, ThreadFlags,
    ProcessParameters,    // from RtlCreateProcessParametersEx
    CreateInfo,
    AttributeList
)
```

This is the lowest-level process creation API. The Win32 `CreateProcessW` wrapper in kernel32 is typically more convenient.

### ZwQueryInformationProcess

Query process information. **5 arguments:**

```
ZwQueryInformationProcess(ProcessHandle, ProcessInformationClass,
                          ProcessInformation, ProcessInformationLength,
                          ReturnLength)
```

Used with `ProcessDeviceMap` (class 23) to enumerate drive letter mappings.

---

## Synchronization

### ZwCreateEvent

Create a synchronization event object. **5 arguments:**

```
ZwCreateEvent(EventHandle, DesiredAccess, ObjectAttributes,
              EventType, InitialState)
```

- `EventType` — `NotificationEvent` (manual reset) or `SynchronizationEvent` (auto reset)

### ZwWaitForSingleObject

Wait for an object (event, process, thread, etc.) to be signaled. **3 arguments:**

```
ZwWaitForSingleObject(Object, Alertable, Timeout)
```

- `Timeout` — `LARGE_INTEGER` in 100ns units; negative = relative, NULL = infinite

---

## Device I/O

### ZwDeviceIoControlFile

Send a control code to a device driver. **10 arguments:**

```
ZwDeviceIoControlFile(
    FileHandle,            // device handle
    Event,                 // optional completion event
    ApcRoutine, ApcContext,
    IoStatusBlock,
    IoControlCode,         // IOCTL code
    InputBuffer, InputBufferLength,
    OutputBuffer, OutputBufferLength
)
```

Used for network socket operations (via AFD driver), device queries, and other driver communication.

---

## Pipe Operations

### ZwCreateNamedPipeFile

Create a named pipe. **14 arguments** — the largest syscall wrapper:

```
ZwCreateNamedPipeFile(
    FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
    ShareAccess, CreateDisposition, CreateOptions,
    NamedPipeType,       // byte-stream or message
    ReadMode,            // byte-stream or message
    CompletionMode,      // blocking or non-blocking
    MaximumInstances,    // max concurrent pipe instances
    InboundQuota,        // input buffer size
    OutboundQuota,       // output buffer size
    DefaultTimeout       // default timeout for pipe operations
)
```

---

## Object Management

### ZwClose

Close any NT object handle. **1 argument.** Used for files, events, processes, threads, pipes, etc.

### ZwSetInformationObject

Modify handle attributes. **4 arguments:**

```
ZwSetInformationObject(Handle, ObjectInformationClass,
                       ObjectInformation, ObjectInformationLength)
```

---

## System Information

### ZwQuerySystemInformation

Query system-wide information. **4 arguments:**

```
ZwQuerySystemInformation(SystemInformationClass, SystemInformation,
                         SystemInformationLength, ReturnLength)
```

Used with `SystemFirmwareTableInformation` (class 76) to retrieve SMBIOS data for hardware fingerprinting (system UUID, manufacturer, product name).

---

## Runtime Library Functions (Rtl*)

These are not syscalls — they are regular functions exported by ntdll.dll. Their addresses are resolved via [PEB export resolution](PEB_WALKING.md) and called directly.

### RtlDosPathNameToNtPathName_U

Convert a Win32 DOS path to an NT native path:

```
"C:\Windows\System32\cmd.exe"  →  "\??\C:\Windows\System32\cmd.exe"
```

Required because all `Zw*` file operations use NT path format in `OBJECT_ATTRIBUTES`.

### RtlFreeUnicodeString

Free a `UNICODE_STRING` buffer allocated by other Rtl functions (e.g., the NT path buffer from `RtlDosPathNameToNtPathName_U`).

### RtlCreateProcessParametersEx

Create a `RTL_USER_PROCESS_PARAMETERS` structure for `ZwCreateUserProcess`. Sets up:
- Image path
- Command line
- Current directory
- Environment block
- Window title, desktop info

### RtlDestroyProcessParameters

Free the process parameters structure created by `RtlCreateProcessParametersEx`.

---

## Module Loading

### LdrLoadDll

Load a DLL into the process address space (ntdll's internal loader). **4 arguments:**

```
LdrLoadDll(SearchPath, DllCharacteristics, DllName, BaseAddress)
```

- Used as the slow path in `ResolveExportAddress` when a module is not already loaded
- This is the lowest-level DLL loading function — `LoadLibraryW` in kernel32 wraps this

---

## Error Handling

All `Zw*` wrappers return `Result<NTSTATUS, Error>`:

```c
return result::FromNTSTATUS<NTSTATUS>(status);
```

- `status >= 0` → `Result::Ok(status)` (NT_SUCCESS)
- `status < 0` → `Result::Err(Error::Windows(status))` (NT failure)

Common NTSTATUS values:

| Value | Name | Meaning |
|---|---|---|
| `0x00000000` | `STATUS_SUCCESS` | Operation succeeded |
| `0x00000103` | `STATUS_PENDING` | Operation in progress (async I/O) |
| `0xC0000022` | `STATUS_ACCESS_DENIED` | Insufficient privileges |
| `0xC0000034` | `STATUS_OBJECT_NAME_NOT_FOUND` | File/object not found |
| `0xC0000035` | `STATUS_OBJECT_NAME_COLLISION` | File/object already exists |
| `0xC0000008` | `STATUS_INVALID_HANDLE` | Bad handle value |
| `0xC000000D` | `STATUS_INVALID_PARAMETER` | Bad parameter |

---

[< Back to Windows Kernel README](README.md) | [Previous: Indirect Syscalls](INDIRECT_SYSCALLS.md) | [Next: Win32 Wrappers >](WIN32_WRAPPERS.md)
