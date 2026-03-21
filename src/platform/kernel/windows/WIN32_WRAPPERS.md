# Win32 API Wrappers

[< Back to Windows Kernel README](README.md)

**Files:** [`kernel32.h`](kernel32.h), [`kernel32.cc`](kernel32.cc), [`user32.h`](user32.h), [`user32.cc`](user32.cc), [`gdi32.h`](gdi32.h), [`gdi32.cc`](gdi32.cc)

Win32 DLL wrappers for process management, display enumeration, and graphics operations. All function addresses resolved dynamically at call time via DJB2 hash-based [PEB module lookup](PEB_WALKING.md) and [PE export resolution](PE_PARSING.md).

---

## Table of Contents

- [Resolution Pattern](#resolution-pattern)
- [Kernel32 — Process and Handle Management](#kernel32--process-and-handle-management)
- [User32 — Display Enumeration](#user32--display-enumeration)
- [GDI32 — Graphics Device Interface](#gdi32--graphics-device-interface)
- [Module Loading Notes](#module-loading-notes)
- [Types and Constants](#types-and-constants)

---

## Resolution Pattern

Each DLL wrapper module defines a resolution macro that resolves function addresses at call time:

```c
// kernel32.cc
#define ResolveKernel32ExportAddress(functionName) \
    ResolveExportAddress(L"kernel32.dll", Djb2::HashCompileTime(functionName))

// user32.cc
#define ResolveUser32ExportAddress(functionName) \
    ResolveExportAddress(L"user32.dll", Djb2::HashCompileTime(functionName))

// gdi32.cc
#define ResolveGdi32ExportAddress(functionName) \
    ResolveExportAddress(L"gdi32.dll", Djb2::HashCompileTime(functionName))
```

Each wrapper then casts the resolved address to the correct function pointer type and calls it:

```c
Result<void, Error> Kernel32::CreateProcessW(/* params */)
{
    BOOL result = ((BOOL(STDCALL *)(PWCHAR, PWCHAR, PVOID, PVOID, BOOL,
                   UINT32, PVOID, PWCHAR, LPSTARTUPINFOW,
                   LPPROCESS_INFORMATION))
                   ResolveKernel32ExportAddress("CreateProcessW"))
                   (lpApplicationName, lpCommandLine, ...);

    if (!result)
        return Result<void, Error>::Err(Error(Error::Kernel32_CreateProcessFailed));
    return Result<void, Error>::Ok();
}
```

---

## Kernel32 — Process and Handle Management

### CreateProcessW

Creates a new process and its primary thread.

```c
Result<void, Error> Kernel32::CreateProcessW(
    PWCHAR lpApplicationName,       // path to executable (or NULL)
    PWCHAR lpCommandLine,           // command line string
    PVOID lpProcessAttributes,      // security attributes for process (NULL = default)
    PVOID lpThreadAttributes,       // security attributes for thread (NULL = default)
    BOOL bInheritHandles,           // inherit handles from calling process
    UINT32 dwCreationFlags,         // CREATE_NO_WINDOW, CREATE_SUSPENDED, etc.
    PVOID lpEnvironment,            // environment block (NULL = inherit parent's)
    PWCHAR lpCurrentDirectory,      // working directory (NULL = caller's)
    LPSTARTUPINFOW lpStartupInfo,   // window/handle configuration
    LPPROCESS_INFORMATION lpProcessInformation  // OUT: process/thread handles and IDs
);
```

**Typical usage for shell command execution with I/O redirection:**

```
1. CreatePipe() for stdin, stdout, stderr
2. SetHandleInformation() to make pipe ends inheritable
3. Set up STARTUPINFOW with STARTF_USESTDHANDLES
4. CreateProcessW() with bInheritHandles = TRUE
5. Read/write via pipe handles
```

**Supporting structures:**

`STARTUPINFOW` — configures the new process's window and standard handles:

| Field | Purpose |
|---|---|
| `cb` | Size of structure (must be set) |
| `dwFlags` | Which fields to use (`STARTF_USESTDHANDLES`) |
| `hStdInput` | Standard input handle |
| `hStdOutput` | Standard output handle |
| `hStdError` | Standard error handle |
| `lpDesktop` | Desktop name (NULL = default) |
| `wShowWindow` | Window show state (if `STARTF_USESHOWWINDOW`) |

`PROCESS_INFORMATION` — receives identification info about the new process:

| Field | Purpose |
|---|---|
| `hProcess` | Handle to the new process |
| `hThread` | Handle to the primary thread |
| `dwProcessId` | Process ID |
| `dwThreadId` | Thread ID |

### SetHandleInformation

Sets properties on an object handle — primarily used to control handle inheritance:

```c
Result<void, Error> Kernel32::SetHandleInformation(
    PVOID hObject,    // handle to modify
    UINT32 dwMask,    // which flags to change (HANDLE_FLAG_INHERIT)
    UINT32 dwFlags    // new flag values
);
```

**Example:** Make a pipe handle non-inheritable so the child process doesn't receive it:

```c
Kernel32::SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);
```

### CreatePipe

Creates an anonymous pipe for inter-process communication:

```c
Result<void, Error> Kernel32::CreatePipe(
    PPVOID hReadPipe,           // OUT: read end of the pipe
    PPVOID hWritePipe,          // OUT: write end of the pipe
    PVOID lpPipeAttributes,     // security attributes (NULL = non-inheritable)
    UINT32 nSize                // suggested buffer size (0 = system default)
);
```

### PeekNamedPipe

Non-blocking check for available data in a pipe:

```c
Result<void, Error> Kernel32::PeekNamedPipe(
    SSIZE hNamedPipe,                // pipe handle
    PVOID lpBuffer,                  // optional output buffer (can be NULL)
    UINT32 nBufferSize,              // buffer size
    PUINT32 lpBytesRead,             // OUT: bytes copied (can be NULL)
    PUINT32 lpTotalBytesAvail,       // OUT: total bytes available (can be NULL)
    PUINT32 lpBytesLeftThisMessage   // OUT: bytes left in message (can be NULL)
);
```

Used to implement non-blocking reads and timeouts on pipe handles.

---

## User32 — Display Enumeration

### EnumDisplayDevicesW

Enumerates display adapters and monitors attached to the system:

```c
BOOL User32::EnumDisplayDevicesW(
    const WCHAR *lpDevice,             // NULL for adapters, device name for monitors
    UINT32 iDevNum,                    // zero-based index
    PDISPLAY_DEVICEW lpDisplayDevice,  // OUT: device info (cb must be set)
    UINT32 dwFlags                     // reserved, set to 0
);
```

**Enumeration pattern:**

```
// Enumerate adapters
for (i = 0; EnumDisplayDevicesW(NULL, i, &dev, 0); i++) {
    if (dev.StateFlags & DISPLAY_DEVICE_ACTIVE) {
        // Active display adapter at dev.DeviceName (e.g., "\\\\.\\DISPLAY1")
    }
}
```

**DISPLAY_DEVICEW structure:**

| Field | Size | Purpose |
|---|---|---|
| `cb` | 4 | Size of structure (must be set before calling) |
| `DeviceName` | 64 | Device name (e.g., `"\\\\.\\DISPLAY1"`) |
| `DeviceString` | 256 | Description (e.g., `"NVIDIA GeForce RTX 4090"`) |
| `StateFlags` | 4 | `DISPLAY_DEVICE_ACTIVE`, `DISPLAY_DEVICE_PRIMARY_DEVICE` |
| `DeviceID` | 256 | PnP device interface ID |
| `DeviceKey` | 256 | Registry key path |

### EnumDisplaySettingsW

Queries display resolution, refresh rate, and orientation:

```c
BOOL User32::EnumDisplaySettingsW(
    const WCHAR *lpszDeviceName,  // device name from DISPLAY_DEVICEW, or NULL
    UINT32 iModeNum,              // mode index, or ENUM_CURRENT_SETTINGS (-1)
    PDEVMODEW lpDevMode           // OUT: display settings (dmSize must be set)
);
```

**DEVMODEW key fields:**

| Field | Purpose |
|---|---|
| `dmPelsWidth` | Display width in pixels |
| `dmPelsHeight` | Display height in pixels |
| `dmBitsPerPel` | Color depth (bits per pixel) |
| `dmDisplayFrequency` | Refresh rate in Hz |
| `dmPositionX/Y` | Display position (for multi-monitor setups) |
| `dmDisplayOrientation` | Rotation (0, 90, 180, 270) |

### GetDC / ReleaseDC

Get and release a device context for drawing operations:

```c
PVOID User32::GetDC(PVOID hWnd);          // NULL = entire screen
INT32 User32::ReleaseDC(PVOID hWnd, PVOID hDC);
```

The screen DC is used with GDI functions for screen capture.

---

## GDI32 — Graphics Device Interface

GDI32 provides bitmap creation, device context management, and pixel data extraction — primarily used for screen capture.

### Screen Capture Flow

```
1. User32::GetDC(NULL)                    → screen DC
2. Gdi32::CreateCompatibleDC(screenDC)    → memory DC
3. Gdi32::CreateCompatibleBitmap(screenDC, w, h)  → bitmap
4. Gdi32::SelectObject(memDC, bitmap)     → select bitmap into memory DC
5. Gdi32::BitBlt(memDC, 0,0,w,h, screenDC, x,y, SRCCOPY)  → copy pixels
6. Gdi32::GetDIBits(memDC, bitmap, 0, h, buffer, &bmi, DIB_RGB_COLORS)  → extract
7. Cleanup: DeleteObject, DeleteDC, ReleaseDC
```

### Function Reference

| Function | Purpose |
|---|---|
| `CreateCompatibleDC(hdc)` | Create memory DC compatible with given DC |
| `CreateCompatibleBitmap(hdc, cx, cy)` | Create bitmap of specified dimensions |
| `SelectObject(hdc, h)` | Select GDI object (bitmap, brush, etc.) into DC |
| `BitBlt(hdc, x,y,cx,cy, hdcSrc, x1,y1, rop)` | Bit-block transfer between DCs |
| `GetDIBits(hdc, hbm, start, lines, bits, bmi, usage)` | Extract pixel data from bitmap |
| `DeleteDC(hdc)` | Delete a device context |
| `DeleteObject(ho)` | Delete a GDI object (bitmap, brush, font, etc.) |

### BITMAPINFOHEADER

Used with `GetDIBits` to specify desired output format:

| Field | Typical Value | Purpose |
|---|---|---|
| `biSize` | `sizeof(BITMAPINFOHEADER)` | Structure size |
| `biWidth` | display width | Bitmap width |
| `biHeight` | `-displayHeight` | Negative = top-down bitmap |
| `biPlanes` | `1` | Must be 1 |
| `biBitCount` | `32` | 32-bit BGRA |
| `biCompression` | `BI_RGB` (0) | Uncompressed |

### GDI Constants

| Constant | Value | Purpose |
|---|---|---|
| `SRCCOPY` | `0x00CC0020` | Copy source pixels directly (raster op) |
| `BI_RGB` | `0` | Uncompressed bitmap format |
| `DIB_RGB_COLORS` | `0` | Color table contains RGB values |

---

## Module Loading Notes

**kernel32.dll** is always loaded — it's one of the first modules in every Windows process. Resolution via `ResolveExportAddress` will always hit the fast path (PEB lookup).

**user32.dll** and **gdi32.dll** may not be loaded by default in console applications or minimal processes. `ResolveExportAddress` handles this automatically:
1. Fast path: check PEB module list
2. Slow path: if not found, call `NTDLL::LdrLoadDll` to load the DLL
3. Then resolve the export from the newly loaded module

---

## Types and Constants

### Handle Flags

| Constant | Value | Purpose |
|---|---|---|
| `HANDLE_FLAG_INHERIT` | `0x01` | Handle can be inherited by child processes |

### Startup Flags

| Constant | Value | Purpose |
|---|---|---|
| `STARTF_USESTDHANDLES` | `0x100` | Use `hStdInput/Output/Error` from `STARTUPINFOW` |

### Display Device Flags

| Constant | Value | Purpose |
|---|---|---|
| `DISPLAY_DEVICE_ACTIVE` | `0x01` | Device is active/attached |
| `DISPLAY_DEVICE_PRIMARY_DEVICE` | `0x04` | Primary display device |
| `ENUM_CURRENT_SETTINGS` | `(UINT32)-1` | Query current active display mode |

---

[< Back to Windows Kernel README](README.md) | [Previous: NTDLL Wrappers](NTDLL_WRAPPERS.md)
