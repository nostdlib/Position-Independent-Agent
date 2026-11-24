[< Back to Platform](../README.md) | [< Back to Project Root](../../../README.md)

# Filesystem Operations

Platform-independent filesystem abstraction for file I/O, directory enumeration, and path manipulation. All operations go through raw kernel syscalls — no `fopen`, no `opendir`, no `FindFirstFile`.

## RAII File Handle

The `File` class wraps an OS file handle with automatic cleanup. Move-only, stack-only, non-copyable — the destructor calls `Close()` so handles never leak.

### Path Conversion

Every platform uses a different path format internally:
- **POSIX**: UTF-8 byte strings with `/` separators
- **Windows**: NT path format (`\??\C:\file.txt`) via `RtlDosPathNameToNtPathName_U`
- **UEFI**: Wide strings with `\` separators

The `NormalizePath` utility converts the runtime's wide-string paths to the platform-native format before passing to syscalls.

### 32-bit lseek Workaround

On 32-bit targets the plain `lseek` syscall takes a pointer-sized `off_t`, so file positions past 4 GiB wrap (and downloads of larger files never terminate). Linux 32-bit architectures therefore use `sys_llseek` with **5 arguments** — the 64-bit offset is split into high/low 32-bit halves, and the result is written to a pointer:

```c
// sys_llseek(fd, offset_high, offset_low, &result, whence)
System::Call(SYS_LSEEK64, fd, offsetHigh, offsetLow, (USIZE)&result64, whence);
```

- **riscv32**: `SYS_LSEEK64` = 62 — the generic syscall table's `lseek` already *is* `sys_llseek`
- **i386 / armv7a**: `SYS_LSEEK64` = 140 (`SYS_LSEEK` 19 takes a 32-bit `off_t`)
- **mips (o32)**: `SYS_LSEEK64` = 4140 (`SYS_LSEEK` 4019 takes a 32-bit `off_t`)
- **FreeBSD i386**: `SYS_LSEEK` (478) takes the 64-bit `off_t` split across two consecutive words — `(fd, offset_lo, offset_hi, whence)` — a framing validated by CI's FreeBSD VM; the position comes back in `eax`, so offsets in 2–4 GiB read back as negative and 4 GiB+ wraps on this target (a `edx:eax`-capturing variant was attempted and broke every seek in CI — needs a real FreeBSD i386 system to develop against)
- **64-bit targets**: plain 3-argument `lseek` (`off_t` is already 64-bit)
- **macOS / Solaris i386**: no 64-bit `llseek` variant exists in their syscall tables, so the plain call (and its 32-bit `off_t` limit) remains

The `File` API is 64-bit throughout (`SetOffset(UINT64)`, `GetOffset() → UINT64`, `GetSize() → UINT64`) precisely so >4 GiB transfers behave identically on 32- and 64-bit agents — on every platform except macOS/Solaris i386 and FreeBSD i386, whose kernel ABIs cannot carry a full 64-bit seek/position (FreeBSD i386 framing is CI-validated only to main's coverage).

## Directory Enumeration Internals

`DirectoryIterator` is the most platform-divergent component in the entire runtime. Each OS returns directory entries in a different structure layout, and metadata retrieval varies wildly.

### Buffered Reading

All platforms read directory entries into a stack buffer (typically 4096 bytes) in one syscall, then parse entries out of the buffer:

```
getdents64(dirfd, buffer[4096], 4096) → bytes_read
  │
  ├─ entry 0: [ino|off|reclen|type|name\0|padding]
  ├─ entry 1: [ino|off|reclen|type|name\0|padding]  ← variable-size records
  └─ entry 2: ...

Advance: ptr += entry->Reclen  (each entry self-describes its size)
```

When all entries in the buffer are consumed, another syscall fills the buffer with the next batch.

### The `struct stat` Offset Problem

To get file size, timestamps, and attributes, each entry needs a `fstatat` call. But `struct stat` has a **different memory layout on every architecture and platform** — field offsets vary because of alignment, field sizes, and historical ABI decisions.

The code uses hardcoded offsets instead of a `struct stat` definition (which would require platform headers):

| Platform | Architecture | `st_mode` offset | `st_size` offset | `st_mtime` offset |
|---|---|---|---|---|
| Linux | x86_64 | 24 | 48 | 88 |
| Linux | i386 / ARMv7-A | 16 | 44 | 72 |
| Linux | MIPS64 | 24 | 56 | 72 |
| macOS | all | 4 | 96 | 48 |
| FreeBSD | i386 | 24 | 96 | 60 |
| FreeBSD | LP64 | 24 | 112 | 64 |
| Solaris | i386 | 20 | 44 | 72 |
| Solaris | x86_64 | 16 | 48 | 88 |

This avoids any dependency on system headers while correctly parsing the stat buffer on each platform.

### Solaris: No `d_type` Field

Solaris `dirent` lacks a `d_type` field (unlike Linux/BSD). To determine if an entry is a file or directory, a **separate `fstatat` call is required for every entry** — making Solaris directory iteration significantly more expensive than other platforms.

Additionally, Solaris 64-bit processes get `SIGSYS` on `getdents64` — the code uses `getdents` (the "32-bit" variant) which returns native 64-bit dirents on LP64 processes.

### RISC-V / QEMU Workaround

When running under QEMU user-mode emulation, `O_DIRECTORY` (0x4000 on ARM/RISC-V) is not translated to the host's value. On an x86_64 host, 0x4000 is `O_DIRECT`, causing `openat` to fail. The fix: omit `O_DIRECTORY` for RISC-V and let `getdents64` return `ENOTDIR` for non-directories instead.

### Windows Drive Enumeration

When the path is empty (root), Windows enters a special "drive enumeration" mode:

```
ZwQueryInformationProcess(NtCurrentProcess(), ProcessDeviceMap, ...)
  → PROCESS_DEVICEMAP_INFORMATION.Query.DriveMap = 0b00000000000000000000000000001100
                                                                                  ││
                                                                                  │└─ bit 2 = C: drive
                                                                                  └── bit 3 = D: drive
```

The 26-bit bitmask (A: through Z:) is stored directly in the iterator's handle field. Each `Next()` call finds the next set bit and formats the drive letter as `"X:\"`.

Drive type (fixed, removable, network, ...) comes from the same device-map query: `PROCESS_DEVICEMAP_INFORMATION.Query.DriveType[i]` already carries the Win32 `DRIVE_*` constant per letter, so no extra syscall is needed. If the device-map query fails, `Type` degrades to `DRIVE_UNKNOWN`.

Each local drive also gets a volume serial number (`VolumeSerial` on the entry, 0 when unavailable), fetched with a second round trip per drive:

```
ZwOpenFile("\??\X:\", FILE_READ_ATTRIBUTES | SYNCHRONIZE, ..., FILE_DIRECTORY_FILE)
  → ZwQueryVolumeInformationFile(handle, FileFsVolumeInformation)
      → FILE_FS_VOLUME_INFORMATION.VolumeSerialNumber
  → ZwClose
```

This is the same value `vol X:` prints, and it is stable across drive-letter changes when a removable drive is replugged. The query is best-effort: an empty card-reader slot or a BitLocker-locked volume yields `VolumeSerial = 0`, but the drive entry is still emitted. Only drives the device map positively identifies as `DRIVE_REMOTE` skip the query — opening an unreachable network share can block for the redirector timeout (seconds per drive) inside the synchronous `Next()` call. A degraded device-map query (`Type == DRIVE_UNKNOWN`) still queries: that value means "type unknown", not "unopenable volume".

## Portable Device Pseudo-Roots

MTP/PTP portable devices (phones, cameras) are invisible to the drive bitmask and to kernel mounts — Windows exposes them through the WPD COM API, Linux through GVFS/udisks mounts. Both surfaces below feed the **empty-path root listing** and are then browsed/read through the ordinary `DirectoryIterator`/`File` APIs: no wire-format change, no new opcode, no capability bit.

### Windows: the `::mtp-` scheme (`fs/windows/wpd.cc`)

A device root entry is emitted as:

```
::mtp-<16 lowercase hex>[-<friendly name>]
```

- The 64-bit **token** is `Djb2::Hash` of the PnP device id. Navigation (`WPD::TryParsePath` → `BeginObjectEnumeration`) re-enumerates `GetDevices` and hash-matches the token — first match wins; no match classifies as `Fs_DeviceGone`.
- The **friendly name** is display-only and never parsed (grammar-unsafe characters become `_`). Path parsing validates the hex strictly; anything else falls through to the NT layer and fails exactly as before.
- Subpaths split on `\` only and are matched segment by segment (exact match first, then case-insensitive) from `WPD_DEVICE_OBJECT_ID` (`"DEVICE"`); an empty subpath lists the device's storages.
- Entries classify as directories when their `WPD_OBJECT_CONTENT_TYPE` is `WPD_CONTENT_TYPE_FOLDER` **or** `WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT`; device-root children (the storages) are always directories, with or without the property present.
- **Persona skip:** a stick Windows exposes both as a mounted volume and as a portable device would list twice. The Portable Device Enumerator creates `swd#wpdbusenum#…` devices as alternate faces of mounted removable mass-storage volumes — so at `BeginRootEnumeration` the beacon counts those wrapper personas (P, an id-prefix check) and the lettered volumes on removable USB media (V, `IOCTL_STORAGE_QUERY_PROPERTY` on each volume's bare device handle `\??\X:` — root-directory handles reject storage IOCTLs with `STATUS_INVALID_PARAMETER`), and skips the wrappers when `V ≥ P`. V counts removable media only: a lettered fixed-type volume (external HDD bridge reporting non-removable) draws no persona and must not inflate the count, and aliases — one volume mounted at two letters — dedupe by volume serial so they count once. `V ≥ P` is a **count inference, not a per-device match** — the wrapper id itself is opaque (a random `{guid}`, no serial or inquiry identity — hardware-verified on Win11 26300), which is why the skip keys on the enumerator prefix and counts rather than matching identities. The inference assumes every persona wraps a distinct lettered removable USB volume and that no lettered removable volume without a persona inflates V. When that assumption fails in the surplus direction — e.g. a removable volume whose persona is absent (WPD service disabled, persona creation lagging) alongside a letterless persona such as a BitLocker-locked stick, whose persona is its only listing — the skip can suppress that listing: the one outcome that hides a device. In the deficit direction (a persona whose volume reports non-removable media or a non-USB bus — internal card reader, Thunderbolt) V stays below P and the duplicate is kept. When `V < P` all personas are kept — a duplicate beats a hidden device. Every open/query failure merely undercounts V, which can only disable the skip. Phones are expected on their own buses (`usb#vid_…`), never under wpdbusenum (unvalidated on hardware); one appearing there would merely inflate P and turn the skip off (duplicate, nothing hidden). Hardware validation should specifically cover: a phone attached alongside a mounted stick, a stick reporting non-removable media (expect its persona kept, duplicate listed), and a letterless locked stick alongside a persona-less removable volume.

COM is hand-declared C-style (plain structs + full SDK-order vtables, the UEFI protocol idiom) — no SDK headers, no imports. Exports resolve per call via `Com` (`combase.dll` first, `ole32.dll` fallback). COM init/uninit is balanced per iterator/stream state object; device open sends neutral client values (`WPD_CLIENT_NAME/MAJOR/MINOR/REVISION`).

Scope is **read-only**: `File::Open` with any write/create/truncate flag on a `::mtp-` path fails with `0x80070005` (`E_ACCESSDENIED`). `File::Exists`/`File::Delete` are not routed and fail at the NT layer.

| HRESULT (root cause) | Condition | Wire cause |
|---|---|---|
| `0x80070651` (`ERROR_DEVICE_REMOVED`) | device id no longer present | `Fs_DeviceGone` |
| `0x8007048F` (`ERROR_DEVICE_NOT_CONNECTED`) | device disconnected | `Fs_DeviceGone` |
| `0x80070037` (`ERROR_DEV_NOT_EXIST`) | device no longer exists | `Fs_DeviceGone` |
| `0x80070002`/`0x80070003` | object/path segment not found | `Fs_PathNotFound` |
| `0x80070490` (`ERROR_NOT_FOUND`) | element not found | `Fs_PathNotFound` |
| `0x80070005` (`E_ACCESSDENIED`) | write on read-only pseudo-root | `Fs_AccessDenied` |

Known limitations (deliberate): object names containing `\` or `/` render in listings but are not addressable (the separator *is* the grammar); duplicate names resolve to the first match; an object whose properties cannot be read is still listed, under its raw object id as the name, but is not navigable (path resolution matches by name); a device removed mid-session classifies as `Fs_DeviceGone` on the next operation.

Devices with non-seekable WPD resource streams are supported: the stream position is tracked internally, and a backward seek re-opens the resource stream and discards forward to the requested offset.

### Linux: GVFS/udisks mount roots (`fs/posix/portable_roots.cc`)

The empty-path root listing appends portable mounts after the real `/` entries (drained one per `Next()` at the clean end):

- `CollectMountedMedia`: depth-2 scan of `/media` and `/run/media` (`/media/<user>/<label>/`, the udisks layout), iterated with `DirectoryIterator` itself.
- `CollectGvfsMounts`: `/run/user/<numeric-uid>/gvfs/` children named `mtp:*` or `gphoto2:*` (GVFS FUSE mounts). Other users' directories fail with EACCES and are skipped silently; non-numeric uid names are ignored.

Entries are real paths with a trailing `/` (e.g. `/run/user/1000/gvfs/mtp:host=.../`), flagged `IsDrive=TRUE`, `Type=2` (removable), `VolumeSerial=0`. Because they are real paths, browsing/reading them flows through the ordinary POSIX iterator unchanged; a stale mount fails like any missing path.

### Other platforms

macOS, iOS, UEFI, Solaris, FreeBSD, Android: out of scope — no portable-device roots are collected and the root listing is unchanged.

## Path Manipulation

All `Path` methods are `static constexpr` — no heap allocations, no syscalls. Platform separator (`/` vs `\`) is a compile-time constant:

```c
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_UEFI)
constexpr CHAR PATH_SEPARATOR = '\\';
#else
constexpr CHAR PATH_SEPARATOR = '/';
#endif
```

## UEFI Filesystem Access

UEFI accesses files through protocol interfaces, not syscalls:

```
LocateProtocol(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID)
  → fsProtocol→OpenVolume(&rootDir)
    → rootDir→Open(&fileHandle, path, mode, attributes)
      → fileHandle→Read/Write/SetPosition/GetInfo
```

GUIDs are constructed field-by-field on the stack (not from `.rdata`) to maintain position independence:

```c
NOINLINE EFI_GUID MakeFsProtocolGuid() {
    EFI_GUID g;
    g.Data1 = 0x964E5B22;
    g.Data2 = 0x6459;
    g.Data3 = 0x11D2;
    g.Data4[0] = 0x8E; g.Data4[1] = 0x39; ...
    return g;
}
```

The `NOINLINE` attribute prevents the compiler from constant-folding the GUID into a `.rdata` section.
