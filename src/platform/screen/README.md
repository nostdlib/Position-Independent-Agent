[< Back to Platform](../README.md) | [< Back to Project Root](../../../README.md)

# Screen Capture

Platform-independent display enumeration and framebuffer capture. Five fundamentally different graphics subsystems abstracted behind a single `Screen::Capture()` interface.

## Windows: GDI BitBlt Pipeline

The classic Win32 screen capture approach, but implemented without linking to any DLLs:

```
GetDC(NULL)                          → screen DC (entire virtual desktop)
  │
  CreateCompatibleDC(screenDC)       → memory DC (offscreen buffer)
  CreateCompatibleBitmap(w, h)       → bitmap matching screen format
  SelectObject(memDC, bitmap)        → target bitmap into memory DC
  │
  BitBlt(memDC, 0, 0, w, h,         → copy pixels from screen to memory
         screenDC, x, y, SRCCOPY)      (handles multi-monitor offsets)
  │
  GetDIBits(memDC, bitmap, 0, h,     → extract pixel data into buffer
            buffer, &bmi,              (converts to 32-bit BGRA top-down)
            DIB_RGB_COLORS)
  │
  Cleanup: DeleteObject, DeleteDC, ReleaseDC
```

Multi-monitor support: `EnumDisplayDevicesW` iterates adapters, `EnumDisplaySettingsW` gets resolution, and `DEVMODEW.dmPositionX/Y` provides the virtual desktop offset for `BitBlt`.

## Linux: Three-Tier Capture Strategy

Linux has no single standard screen capture API. The runtime tries three methods in order:

### Tier 1: X11 Protocol (Display Server)

Direct X11 protocol communication via Unix socket — no libX11 dependency:

1. Parse `~/.Xauthority` for MIT-MAGIC-COOKIE-1 authentication
2. Connect to X11 socket (`/tmp/.X11-unix/X{display}`)
3. Send `GetImage` request (opcode 73) in `ZPixmap` format
4. Receive raw pixel data

Detected displays are encoded with a sentinel: `Left = -(1000 + displayNum)`.

### Tier 2: DRM Dumb Buffers (Kernel Mode-Setting)

Direct framebuffer access through the DRM (Direct Rendering Manager) subsystem:

```
open("/dev/dri/card0")
  → ioctl(DRM_IOCTL_MODE_GETRESOURCES)     → list of CRTCs, connectors, encoders
  → ioctl(DRM_IOCTL_MODE_GETCRTC)          → active framebuffer ID
  → ioctl(DRM_IOCTL_MODE_MAP_DUMB)         → mmap offset for framebuffer
  → mmap(offset)                            → pixel data
```

**GPU-composited scanout detection:** After capturing, the code checks if the buffer is all-black — which indicates a GPU-composited framebuffer that can't be read via dumb buffers. If detected, falls through to fbdev.

Encoded as `Left = -(cardIndex + 1)`, `Top = crtcId`.

### Tier 3: fbdev (Linux Framebuffer)

The legacy fallback, using `/dev/fb0` through `/dev/fb7`:

```
ioctl(FBIOGET_FSCREENINFO)   → line_length, smem_len (framebuffer size)
ioctl(FBIOGET_VSCREENINFO)   → xres, yres, bits_per_pixel
mmap(0, smem_len)            → direct pixel access
```

**Android variant:** Tries `/dev/graphics/fb0..fb7` when standard `/dev/fb*` paths are unavailable.

## macOS: CoreGraphics via dyld with Fork-Based Probing

No link-time dependency on CoreGraphics. Functions are resolved at runtime through the dyld framework loader:

```c
dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
dlsym(handle, "CGMainDisplayID")
dlsym(handle, "CGDisplayBounds")
dlsym(handle, "CGDisplayCreateImage")
// ... etc.
```

### Fork-Based Crash Isolation

CoreGraphics may crash (SIGKILL/SIGSYS) on headless systems or sandboxed environments. The runtime isolates this risk using `fork()`:

```
pid = fork()
  │
  ├─ Child: try loading CoreGraphics and calling CGMainDisplayID()
  │         exit(0) if success, exit(1) if failure
  │
  └─ Parent: wait4(pid) and check exit status
             If child crashed → skip CoreGraphics, no capture available
             If child exited 0 → safe to use CoreGraphics in parent
```

### Pixel Format Detection

CoreGraphics returns images in various pixel formats. The code decodes `CGBitmapInfo`:

```c
alphaInfo = bitmapInfo & 0x1F;    // alpha/skip mask
byteOrder = bitmapInfo & 0xF000;  // endian indicator

// Map to RGB byte offsets:
// BGRA (little-endian) → rOff=2, gOff=1, bOff=0
// RGBA (big-endian)    → rOff=0, gOff=1, bOff=2
```

**Retina handling:** `CGDisplayCreateImage` may return a 2x image. The code clamps pixel dimensions to the device's reported resolution.

## Solaris: SunOS Framebuffer

Simple framebuffer access via `/dev/fb`:

```
open("/dev/fb")
ioctl(FBIOGTYPE)   → display type, width, height, depth
mmap(size)         → raw pixel data
```

Single framebuffer only — no multi-monitor support.

## UEFI: Graphics Output Protocol

```
LocateProtocol(EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID) → gop
gop→QueryMode(mode) → resolution, pixel format, pixels_per_scanline
gop→Blt(buffer, EfiBltVideoToBltBuffer, ...) → copy framebuffer to buffer
```

Pixel formats vary by firmware: `PixelRedGreenBlueReserved8BitPerColor` (RGBX) or `PixelBlueGreenRedReserved8BitPerColor` (BGRX). The code handles both.

## Platform Support

| Platform | Method | Multi-Monitor | Notes |
|---|---|---|---|
| Windows | GDI BitBlt | Yes | User32 + GDI32 via PEB resolution |
| Linux | X11 / DRM / fbdev | Yes (X11/DRM) | Three-tier fallback chain |
| Android | DRM / fbdev | Yes (DRM) | `/dev/graphics/fb*` variant |
| macOS | CoreGraphics | Yes | Fork-based probing, Retina handling |
| Solaris | `/dev/fb` ioctl | No | Single framebuffer |
| UEFI | GOP BLT | No | Firmware-provided framebuffer |
| iOS | — | — | Not implemented (sandbox restriction) |
