/**
 * @file screen.cc
 * @brief POSIX Screen Implementation (Linux/Android/FreeBSD/Solaris)
 *
 * @details Implements screen device enumeration and capture via three backends:
 *
 * 1. X11 raw protocol (/tmp/.X11-unix/X<N>): The preferred backend on
 *    Linux desktops running X11 compositors (GNOME, KDE, XFCE, etc.).
 *    Speaks the X11 wire protocol directly over a Unix domain socket —
 *    no libX11 dependency. Authenticates via MIT-MAGIC-COOKIE-1 from
 *    ~/.Xauthority and captures the root window using GetImage (opcode 73)
 *    in ZPixmap format. X11 devices are encoded in ScreenDevice as
 *    Left = -(1000 + displayNum).
 *
 * 2. DRM dumb buffers (/dev/dri/card0..card7): Fallback for non-X11
 *    environments (VMs, TTY consoles, embedded). Enumerates connectors,
 *    encoders, and CRTCs via DRM mode-setting ioctls. Capture maps the
 *    active framebuffer via DRM_IOCTL_MODE_MAP_DUMB. Requires DRM master
 *    or CAP_SYS_ADMIN for framebuffer mapping. DRM devices are encoded in
 *    ScreenDevice as Left = -(cardIndex + 1), Top = crtcId. If the mapped
 *    buffer is all-black (GPU-composited scanout), falls back to
 *    framebuffer.
 *
 * 3. Linux framebuffer (/dev/fb0..fb7): Legacy fallback when both X11 and
 *    DRM are unavailable. Uses FBIOGET_VSCREENINFO and FBIOGET_FSCREENINFO
 *    ioctls with mmap to read pixel data. Shared between Linux, Android,
 *    and FreeBSD (via linuxkpi compatibility). On Android,
 *    /dev/graphics/fb0..fb7 is tried when /dev/fb* is unavailable.
 *    ScreenDevice::Left stores the framebuffer index.
 *
 * GetDevices() tries X11 first (Linux only), then DRM, then framebuffer.
 * Capture() dispatches based on the Left field encoding.
 *
 * Solaris uses the SunOS framebuffer API (sys/fbio.h) with FBIOGTYPE
 * ioctl to query /dev/fb device parameters and mmap to read pixel data.
 * Only a single console framebuffer is supported.
 *
 * macOS and iOS have separate implementations (macos/screen.cc, ios/screen.cc).
 *
 * @see DRM KMS userspace API
 *      https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html
 * @see Linux framebuffer API (linux/fb.h)
 *      https://www.kernel.org/doc/html/latest/fb/api.html
 * @see Solaris fbio(4I)
 *      https://docs.oracle.com/cd/E36784_01/html/E36884/fbio-7i.html
 */

#include "platform/screen/screen.h"
#include "core/memory/memory.h"
#include "platform/system/environment.h"
#if defined(PLATFORM_ANDROID)
#include "platform/system/process.h"
#include "platform/system/pipe.h"
#endif

#if defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#elif defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#elif defined(PLATFORM_FREEBSD)
#include "platform/kernel/freebsd/syscall.h"
#include "platform/kernel/freebsd/system.h"
#elif defined(PLATFORM_SOLARIS)
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"
#endif

#if defined(PLATFORM_SOLARIS)

// =============================================================================
// Solaris framebuffer — FBIOGTYPE ioctl + mmap (/dev/fb)
// =============================================================================

/// @brief SunOS framebuffer type information (sys/fbio.h)
/// @see fbio(4I)
///      https://docs.oracle.com/cd/E36784_01/html/E36884/fbio-7i.html
struct FbType
{
	INT32 Type;	  ///< Frame buffer type (FBTYPE_* constant)
	INT32 Height; ///< Height in pixels
	INT32 Width;  ///< Width in pixels
	INT32 Depth;  ///< Bits per pixel
	INT32 CmSize; ///< Size of color map (entries)
	INT32 Size;	  ///< Total framebuffer memory in bytes
};

/// @brief Get framebuffer type information
/// @details FBIOGTYPE = (FIOC | 0) where FIOC = ('F' << 8)
constexpr USIZE FBIOGTYPE = 0x4600;

/// @brief Open the Solaris console framebuffer device (/dev/fb)
/// @return File descriptor on success, negative errno on failure
static SSIZE OpenFramebuffer()
{
	auto devFb = "/dev/fb";
	CHAR path[8];
	Memory::Copy(path, (const CHAR *)devFb, 8);

	return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)O_RDONLY);
}

/// @brief Perform an ioctl syscall on a file descriptor
/// @param fd File descriptor
/// @param request Ioctl request code
/// @param arg Pointer to request-specific data
/// @return 0 on success, negative errno on failure
static SSIZE Ioctl(SSIZE fd, USIZE request, PVOID arg)
{
	return System::Call(SYS_IOCTL, (USIZE)fd, request, (USIZE)arg);
}

/// @brief Map framebuffer memory for reading
/// @param size Number of bytes to map
/// @param fd Framebuffer file descriptor
/// @return Mapped address, or nullptr on failure
static PVOID MmapFramebuffer(USIZE size, SSIZE fd)
{
	INT32 prot = PROT_READ;
	INT32 flags = MAP_SHARED;

#if defined(ARCHITECTURE_I386)
	// Solaris i386: mmap takes 64-bit off_t split across two 32-bit stack
	// slots. The 6-arg System::Call only pushes one slot for the offset.
	// Use inline asm to push all 7 argument slots + dummy return address.
	SSIZE result;
	register USIZE r1 __asm__("ebx") = 0;			 // addr
	register USIZE r2 __asm__("ecx") = size;		 // len
	register USIZE r3 __asm__("edx") = (USIZE)prot;	 // prot
	register USIZE r4 __asm__("esi") = (USIZE)flags; // flags
	register USIZE r5 __asm__("edi") = (USIZE)fd;	 // fd
	__asm__ volatile(
		"pushl $0\n"	// off_t pos high 32 bits = 0
		"pushl $0\n"	// off_t pos low 32 bits = 0
		"pushl %%edi\n" // fd
		"pushl %%esi\n" // flags
		"pushl %%edx\n" // prot
		"pushl %%ecx\n" // len
		"pushl %%ebx\n" // addr
		"pushl $0\n"	// dummy return address
		"int $0x91\n"
		"jnc 1f\n"
		"negl %%eax\n"
		"1:\n"
		"addl $32, %%esp\n"
		: "=a"(result)
		: "a"((USIZE)SYS_MMAP), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
		: "memory", "cc");
#else
	SSIZE result = System::Call(SYS_MMAP, (USIZE)0, size,
								(USIZE)prot, (USIZE)flags, (USIZE)fd, (USIZE)0);
#endif

	if (result < 0 && result >= -4095)
		return nullptr;

	return (PVOID)result;
}

// =============================================================================
// Screen::GetDevices (Solaris)
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	SSIZE fd = OpenFramebuffer();
	if (fd < 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	FbType fbt;
	Memory::Zero(&fbt, sizeof(fbt));

	SSIZE ret = Ioctl(fd, FBIOGTYPE, &fbt);
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (ret < 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	if (fbt.Width <= 0 || fbt.Height <= 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	ScreenDevice *devices = new ScreenDevice[1];
	if (devices == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_AllocFailed));

	devices[0].Left = 0;
	devices[0].Top = 0;
	devices[0].Width = (UINT32)fbt.Width;
	devices[0].Height = (UINT32)fbt.Height;
	devices[0].Primary = true;

	ScreenDeviceList list;
	list.Devices = devices;
	list.Count = 1;
	return Result<ScreenDeviceList, Error>::Ok(list);
}

// =============================================================================
// Screen::Capture (Solaris)
// =============================================================================

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer)
{
	SSIZE fd = OpenFramebuffer();
	if (fd < 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	FbType fbt;
	Memory::Zero(&fbt, sizeof(fbt));

	SSIZE ret = Ioctl(fd, FBIOGTYPE, &fbt);
	if (ret < 0 || fbt.Width <= 0 || fbt.Height <= 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Validate dimensions match the device
	if ((UINT32)fbt.Width != device.Width || (UINT32)fbt.Height != device.Height)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	UINT32 bytesPerPixel = (UINT32)fbt.Depth / 8;
	if (bytesPerPixel == 0 || bytesPerPixel > 4)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Use fb_size from FBIOGTYPE, fall back to computed size
	USIZE mapSize = (fbt.Size > 0) ? (USIZE)fbt.Size
								   : (USIZE)fbt.Width * (USIZE)fbt.Height * bytesPerPixel;

	PVOID mapped = MmapFramebuffer(mapSize, fd);
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (mapped == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Convert framebuffer pixels to RGB
	// FBIOGTYPE does not report per-component bitfield offsets or line stride,
	// so assume standard little-endian layouts for common depths.
	UINT8 *fbBase = (UINT8 *)mapped;
	PRGB rgbBuf = buffer.Data();
	UINT32 width = device.Width;
	UINT32 height = device.Height;
	USIZE lineLength = (USIZE)width * bytesPerPixel;

	for (UINT32 y = 0; y < height; y++)
	{
		UINT8 *row = fbBase + (USIZE)y * lineLength;

		for (UINT32 x = 0; x < width; x++)
		{
			UINT8 *src = row + (USIZE)x * bytesPerPixel;

			if (bytesPerPixel == 4)
			{
				// 32bpp BGRA (standard x86/aarch64 framebuffer layout)
				rgbBuf[y * width + x].Red = src[2];
				rgbBuf[y * width + x].Green = src[1];
				rgbBuf[y * width + x].Blue = src[0];
			}
			else if (bytesPerPixel == 3)
			{
				// 24bpp BGR
				rgbBuf[y * width + x].Red = src[2];
				rgbBuf[y * width + x].Green = src[1];
				rgbBuf[y * width + x].Blue = src[0];
			}
			else if (bytesPerPixel == 2)
			{
				// 16bpp RGB565
				UINT16 pixel = (UINT16)src[0] | ((UINT16)src[1] << 8);
				rgbBuf[y * width + x].Red = (UINT8)(((pixel >> 11) & 0x1F) * 255 / 31);
				rgbBuf[y * width + x].Green = (UINT8)(((pixel >> 5) & 0x3F) * 255 / 63);
				rgbBuf[y * width + x].Blue = (UINT8)((pixel & 0x1F) * 255 / 31);
			}
		}
	}

	System::Call(SYS_MUNMAP, (USIZE)mapped, mapSize);

	return Result<VOID, Error>::Ok();
}

#elif defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)

// =============================================================================
// Framebuffer ioctl constants
// =============================================================================

/// @brief Get variable screen info (resolution, pixel format)
constexpr USIZE FBIOGET_VSCREENINFO = 0x4600;

/// @brief Get fixed screen info (line length, memory size)
constexpr USIZE FBIOGET_FSCREENINFO = 0x4602;

// =============================================================================
// Framebuffer kernel structures
// =============================================================================

/// @brief Color component bitfield descriptor
struct FbBitfield
{
	UINT32 Offset;	 ///< Bit position of the least significant bit
	UINT32 Length;	 ///< Number of bits in this component
	UINT32 MsbRight; ///< MSB is rightmost (!=0) or leftmost (==0)
};

/// @brief Variable screen information (resolution, pixel format, virtual size)
struct FbVarScreeninfo
{
	UINT32 Xres;		 ///< Visible horizontal resolution in pixels
	UINT32 Yres;		 ///< Visible vertical resolution in pixels
	UINT32 XresVirtual;	 ///< Virtual horizontal resolution
	UINT32 YresVirtual;	 ///< Virtual vertical resolution
	UINT32 Xoffset;		 ///< Horizontal offset into virtual resolution
	UINT32 Yoffset;		 ///< Vertical offset into virtual resolution
	UINT32 BitsPerPixel; ///< Bits per pixel (16, 24, or 32)
	UINT32 Grayscale;	 ///< Non-zero for grayscale displays
	FbBitfield Red;		 ///< Red component bitfield
	FbBitfield Green;	 ///< Green component bitfield
	FbBitfield Blue;	 ///< Blue component bitfield
	FbBitfield Transp;	 ///< Transparency component bitfield
	UINT32 Nonstd;		 ///< Non-standard pixel format flag
	UINT32 Activate;	 ///< Activation flag
	UINT32 HeightMm;	 ///< Height of picture in mm
	UINT32 WidthMm;		 ///< Width of picture in mm
	UINT32 AccelFlags;	 ///< Obsolete acceleration flags
	UINT32 Pixclock;	 ///< Pixel clock in picoseconds
	UINT32 LeftMargin;	 ///< Time from sync to picture (horizontal)
	UINT32 RightMargin;	 ///< Time from picture to sync (horizontal)
	UINT32 UpperMargin;	 ///< Time from sync to picture (vertical)
	UINT32 LowerMargin;	 ///< Time from picture to sync (vertical)
	UINT32 HsyncLen;	 ///< Horizontal sync length
	UINT32 VsyncLen;	 ///< Vertical sync length
	UINT32 Sync;		 ///< Sync type flags
	UINT32 Vmode;		 ///< Video mode flags
	UINT32 Rotate;		 ///< Rotation angle (0, 90, 180, 270)
	UINT32 Colorspace;	 ///< Colorspace for FOURCC-based modes
	UINT32 Reserved[4];	 ///< Reserved for future use
};

/// @brief Fixed screen information (memory layout, line length)
struct FbFixScreeninfo
{
	CHAR Id[16];		 ///< Identification string (e.g. "VESA VGA")
	USIZE SmemStart;	 ///< Start of frame buffer memory (physical address)
	UINT32 SmemLen;		 ///< Length of frame buffer memory in bytes
	UINT32 Type;		 ///< Frame buffer type
	UINT32 TypeAux;		 ///< Interleave for interleaved planes
	UINT32 Visual;		 ///< Visual type (truecolor, pseudocolor, etc.)
	UINT16 Xpanstep;	 ///< Zero if no hardware panning
	UINT16 Ypanstep;	 ///< Zero if no hardware panning
	UINT16 Ywrapstep;	 ///< Zero if no hardware ywrap
	UINT32 LineLength;	 ///< Length of a line in bytes
	USIZE MmioStart;	 ///< Start of memory-mapped I/O (physical address)
	UINT32 MmioLen;		 ///< Length of memory-mapped I/O
	UINT32 Accel;		 ///< Acceleration capabilities
	UINT16 Capabilities; ///< Feature flags
	UINT16 Reserved[2];	 ///< Reserved for future use
};

// =============================================================================
// DRM ioctl constants and structures (/dev/dri/card*)
// =============================================================================

/// @brief DRM mode information (matches kernel struct drm_mode_modeinfo, 68 bytes)
/// @see https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html
struct DrmModeModeinfo
{
	UINT32 Clock;
	UINT16 Hdisplay;
	UINT16 HsyncStart;
	UINT16 HsyncEnd;
	UINT16 Htotal;
	UINT16 Hskew;
	UINT16 Vdisplay;
	UINT16 VsyncStart;
	UINT16 VsyncEnd;
	UINT16 Vtotal;
	UINT16 Vscan;
	UINT32 Vrefresh;
	UINT32 Flags;
	UINT32 Type;
	CHAR Name[32];
};

/// @brief DRM mode card resources (drm_mode_card_res, 64 bytes)
struct DrmModeCardRes
{
	UINT64 FbIdPtr;
	UINT64 CrtcIdPtr;
	UINT64 ConnectorIdPtr;
	UINT64 EncoderIdPtr;
	UINT32 CountFbs;
	UINT32 CountCrtcs;
	UINT32 CountConnectors;
	UINT32 CountEncoders;
	UINT32 MinWidth;
	UINT32 MaxWidth;
	UINT32 MinHeight;
	UINT32 MaxHeight;
};

/// @brief DRM connector information (drm_mode_get_connector, 80 bytes)
struct DrmModeGetConnector
{
	UINT64 EncodersPtr;
	UINT64 ModesPtr;
	UINT64 PropsPtr;
	UINT64 PropValuesPtr;
	UINT32 CountModes;
	UINT32 CountProps;
	UINT32 CountEncoders;
	UINT32 EncoderId;
	UINT32 ConnectorId;
	UINT32 ConnectorType;
	UINT32 ConnectorTypeId;
	UINT32 Connection;
	UINT32 MmWidth;
	UINT32 MmHeight;
	UINT32 Subpixel;
	UINT32 Pad;
};

/// @brief DRM encoder information (drm_mode_get_encoder, 20 bytes)
struct DrmModeGetEncoder
{
	UINT32 EncoderId;
	UINT32 EncoderType;
	UINT32 CrtcId;
	UINT32 PossibleCrtcs;
	UINT32 PossibleClones;
};

/// @brief DRM CRTC information (drm_mode_crtc, 104 bytes)
struct DrmModeCrtc
{
	UINT64 SetConnectorsPtr;
	UINT32 CountConnectors;
	UINT32 CrtcId;
	UINT32 FbId;
	UINT32 X;
	UINT32 Y;
	UINT32 GammaSize;
	UINT32 ModeValid;
	DrmModeModeinfo Mode;
};

/// @brief DRM framebuffer command (drm_mode_fb_cmd, 28 bytes)
struct DrmModeFbCmd
{
	UINT32 FbId;
	UINT32 Width;
	UINT32 Height;
	UINT32 Pitch;
	UINT32 Bpp;
	UINT32 Depth;
	UINT32 Handle;
};

/// @brief DRM map dumb buffer request (drm_mode_map_dumb, 16 bytes)
struct DrmModeMapDumb
{
	UINT32 Handle;
	UINT32 Pad;
	UINT64 Offset;
};

/// @brief DRM GEM close request (drm_gem_close, 8 bytes)
struct DrmGemClose
{
	UINT32 Handle;
	UINT32 Pad;
};

/// @brief DRM connector is attached and active
constexpr UINT32 DRM_MODE_CONNECTED = 1;

/// DRM ioctl numbers: _IOWR('d', nr, struct) = (3<<30)|(sizeof<<16)|(0x64<<8)|nr
/// Note: _IOWR encoding is identical on standard and MIPS (3<<30 == 6<<29 == 0xC0000000)
constexpr USIZE DRM_IOCTL_MODE_GETRESOURCES = 0xC04064A0;
constexpr USIZE DRM_IOCTL_MODE_GETCRTC      = 0xC06864A1;
constexpr USIZE DRM_IOCTL_MODE_GETENCODER   = 0xC01464A6;
constexpr USIZE DRM_IOCTL_MODE_GETCONNECTOR = 0xC05064A7;
constexpr USIZE DRM_IOCTL_MODE_GETFB        = 0xC01C64AD;
constexpr USIZE DRM_IOCTL_MODE_MAP_DUMB     = 0xC01064B3;

/// DRM GEM close: _IOW('d', 0x09, drm_gem_close)
/// MIPS _IOW direction = 4 at bit 29; standard _IOW direction = 1 at bit 30
#if defined(ARCHITECTURE_MIPS64)
constexpr USIZE DRM_IOCTL_GEM_CLOSE         = 0x80086409;
#else
constexpr USIZE DRM_IOCTL_GEM_CLOSE         = 0x40086409;
#endif

// =============================================================================
// Internal helpers — shared
// =============================================================================

/// @brief Open a framebuffer device by index
/// @details Tries /dev/fb<N> first (Linux/FreeBSD). On Android, falls back to
/// /dev/graphics/fb<N> which is the standard Android framebuffer path.
/// @param index Framebuffer device number (0-7)
/// @return File descriptor on success, negative errno on failure
static SSIZE OpenFramebuffer(UINT32 index)
{
	auto devFb = "/dev/fb";
	CHAR path[24];
	Memory::Copy(path, (const CHAR *)devFb, 8);
	path[7] = '0' + (CHAR)index;
	path[8] = '\0';

#if ((defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))) || \
	(defined(PLATFORM_FREEBSD) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64)))
	SSIZE fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)O_RDONLY);
#else
	SSIZE fd = System::Call(SYS_OPEN, (USIZE)path, (USIZE)O_RDONLY);
#endif

#if defined(PLATFORM_ANDROID)
	// Android framebuffer lives at /dev/graphics/fb<N> instead of /dev/fb<N>
	if (fd < 0)
	{
		auto devGfxFb = "/dev/graphics/fb";
		Memory::Copy(path, (const CHAR *)devGfxFb, 17);
		path[16] = '0' + (CHAR)index;
		path[17] = '\0';

#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
		fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)O_RDONLY);
#else
		fd = System::Call(SYS_OPEN, (USIZE)path, (USIZE)O_RDONLY);
#endif
	}
#endif

	return fd;
}

/// @brief Open a DRM card device by index (/dev/dri/card0../dev/dri/card7)
/// @param index DRM card number (0-7)
/// @return File descriptor on success, negative errno on failure
static SSIZE OpenDrmCard(UINT32 index)
{
	auto devDri = "/dev/dri/card";
	CHAR path[16];
	Memory::Copy(path, (const CHAR *)devDri, 14);
	path[13] = '0' + (CHAR)index;
	path[14] = '\0';

#if ((defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))) || \
	(defined(PLATFORM_FREEBSD) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64)))
	return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)O_RDWR);
#else
	return System::Call(SYS_OPEN, (USIZE)path, (USIZE)O_RDWR);
#endif
}

/// @brief Perform an ioctl syscall on a file descriptor
/// @param fd File descriptor
/// @param request Ioctl request code
/// @param arg Pointer to request-specific data
/// @return 0 on success, negative errno on failure
static SSIZE Ioctl(SSIZE fd, USIZE request, PVOID arg)
{
	return System::Call(SYS_IOCTL, (USIZE)fd, request, (USIZE)arg);
}

/// @brief Map framebuffer memory for reading
/// @param size Number of bytes to map
/// @param fd Framebuffer file descriptor
/// @return Mapped address, or nullptr on failure
static PVOID MmapFramebuffer(USIZE size, SSIZE fd)
{
	INT32 prot = PROT_READ;
	INT32 flags = MAP_SHARED;

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A) || defined(ARCHITECTURE_RISCV32))
	// 32-bit Linux/Android uses mmap2 with page-shifted offset
	SSIZE result = System::Call(SYS_MMAP2, (USIZE)0, size,
								(USIZE)prot, (USIZE)flags, (USIZE)fd, (USIZE)0);
#elif defined(PLATFORM_FREEBSD) && defined(ARCHITECTURE_I386)
	// FreeBSD i386: mmap takes 64-bit off_t split across two 32-bit stack
	// slots. System::Call only pushes one slot, so use inline asm to push
	// all 7 argument slots + dummy return address = 32 bytes.
	SSIZE result;
	register USIZE r1 __asm__("ebx") = 0;			 // addr
	register USIZE r2 __asm__("ecx") = size;		 // len
	register USIZE r3 __asm__("edx") = (USIZE)prot;	 // prot
	register USIZE r4 __asm__("esi") = (USIZE)flags; // flags
	register USIZE r5 __asm__("edi") = (USIZE)fd;	 // fd
	__asm__ volatile(
		"pushl $0\n"	// off_t pos high 32 bits = 0
		"pushl $0\n"	// off_t pos low 32 bits = 0
		"pushl %%edi\n" // fd
		"pushl %%esi\n" // flags
		"pushl %%edx\n" // prot
		"pushl %%ecx\n" // len
		"pushl %%ebx\n" // addr
		"pushl $0\n"	// dummy return address
		"int $0x80\n"
		"jnc 1f\n"
		"negl %%eax\n"
		"1:\n"
		"addl $32, %%esp\n"
		: "=a"(result)
		: "a"((USIZE)SYS_MMAP), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
		: "memory", "cc");
#else
	SSIZE result = System::Call(SYS_MMAP, (USIZE)0, size,
								(USIZE)prot, (USIZE)flags, (USIZE)fd, (USIZE)0);
#endif

	if (result < 0 && result >= -4095)
		return nullptr;

	return (PVOID)result;
}

/// @brief Map DRM dumb buffer memory for reading
/// @param size Number of bytes to map
/// @param fd DRM device file descriptor
/// @param offset Offset from DRM_IOCTL_MODE_MAP_DUMB
/// @return Mapped address, or nullptr on failure
static PVOID DrmMmapBuffer(USIZE size, SSIZE fd, UINT64 offset)
{
	INT32 prot = PROT_READ;
	INT32 flags = MAP_SHARED;

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A) || defined(ARCHITECTURE_RISCV32))
	// 32-bit Linux/Android uses mmap2 with page-shifted offset
	SSIZE result = System::Call(SYS_MMAP2, (USIZE)0, size,
		(USIZE)prot, (USIZE)flags, (USIZE)fd, (USIZE)(UINT32)(offset >> 12));
#elif defined(PLATFORM_FREEBSD) && defined(ARCHITECTURE_I386)
	// FreeBSD i386: mmap takes 64-bit off_t split across two 32-bit stack
	// slots. Use inline asm to push all 7 argument slots + dummy return address.
	SSIZE result;
	UINT32 offLo = (UINT32)offset;
	UINT32 offHi = (UINT32)(offset >> 32);
	register USIZE r1 __asm__("ebx") = 0;              // addr
	register USIZE r2 __asm__("ecx") = size;            // len
	register USIZE r3 __asm__("edx") = (USIZE)prot;    // prot
	register USIZE r4 __asm__("esi") = (USIZE)flags;   // flags
	register USIZE r5 __asm__("edi") = (USIZE)fd;      // fd
	__asm__ volatile(
		"movl %[offLo], %%eax\n"  // load offLo before any push
		"pushl %[offHi]\n"        // off_t high (first push, ESP unchanged when read)
		"pushl %%eax\n"           // off_t low (from register)
		"pushl %%edi\n"           // fd
		"pushl %%esi\n"           // flags
		"pushl %%edx\n"           // prot
		"pushl %%ecx\n"           // len
		"pushl %%ebx\n"           // addr
		"pushl $0\n"              // dummy return address
		"movl %[sysno], %%eax\n"  // reload syscall number
		"int $0x80\n"
		"jnc 1f\n"
		"negl %%eax\n"
		"1:\n"
		"addl $32, %%esp\n"
		: "=&a"(result)
		: "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
		  [offLo] "g"(offLo), [offHi] "g"(offHi), [sysno] "i"((int)SYS_MMAP)
		: "memory", "cc"
	);
#else
	SSIZE result = System::Call(SYS_MMAP, (USIZE)0, size,
		(USIZE)prot, (USIZE)flags, (USIZE)fd, (USIZE)offset);
#endif

	if (result < 0 && result >= -4095)
		return nullptr;

	return (PVOID)result;
}

/// @brief Extract an N-bit color component from a pixel value
/// @param pixel Raw pixel value
/// @param field Bitfield descriptor for the component
/// @return 8-bit color value
static UINT8 ExtractComponent(UINT32 pixel, const FbBitfield &field)
{
	if (field.Length == 0)
		return 0;

	UINT32 value = (pixel >> field.Offset) & ((1u << field.Length) - 1);

	// Scale to 8-bit
	if (field.Length < 8)
		value = (value * 255) / ((1u << field.Length) - 1);
	else if (field.Length > 8)
		value >>= (field.Length - 8);

	return (UINT8)value;
}

// =============================================================================
// Internal helpers — DRM enumeration and capture
// =============================================================================

/// @brief Enumerate displays via DRM (/dev/dri/card*)
/// @details Walks card devices, connectors, encoders, and CRTCs to find
/// active displays. DRM devices are encoded in ScreenDevice as:
/// Left = -(cardIndex + 1), Top = crtcId.
/// @param tempDevices Output array for discovered devices
/// @param deviceCount [in/out] Current device count, incremented per device
/// @param maxDevices Maximum capacity of tempDevices
static VOID DrmGetDevices(ScreenDevice *tempDevices, UINT32 &deviceCount, UINT32 maxDevices)
{
	constexpr UINT32 maxCards = 8;
	constexpr UINT32 maxConnectors = 16;

	for (UINT32 cardIdx = 0; cardIdx < maxCards && deviceCount < maxDevices; cardIdx++)
	{
		SSIZE fd = OpenDrmCard(cardIdx);
		if (fd < 0)
			continue;

		// First call: get connector count
		DrmModeCardRes res;
		Memory::Zero(&res, sizeof(res));

		if (Ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
			res.CountConnectors == 0)
		{
			System::Call(SYS_CLOSE, (USIZE)fd);
			continue;
		}

		// Second call: retrieve connector IDs
		UINT32 connCount = res.CountConnectors;
		if (connCount > maxConnectors)
			connCount = maxConnectors;

		UINT32 connectorIds[maxConnectors];
		Memory::Zero(connectorIds, sizeof(connectorIds));

		DrmModeCardRes res2;
		Memory::Zero(&res2, sizeof(res2));
		res2.ConnectorIdPtr = (UINT64)(USIZE)connectorIds;
		res2.CountConnectors = connCount;

		if (Ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res2) < 0)
		{
			System::Call(SYS_CLOSE, (USIZE)fd);
			continue;
		}

		// Walk connectors to find active displays
		for (UINT32 c = 0; c < connCount && deviceCount < maxDevices; c++)
		{
			DrmModeGetConnector conn;
			Memory::Zero(&conn, sizeof(conn));
			conn.ConnectorId = connectorIds[c];

			if (Ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
				continue;

			if (conn.Connection != DRM_MODE_CONNECTED || conn.EncoderId == 0)
				continue;

			// Get encoder to find the CRTC
			DrmModeGetEncoder enc;
			Memory::Zero(&enc, sizeof(enc));
			enc.EncoderId = conn.EncoderId;

			if (Ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0 || enc.CrtcId == 0)
				continue;

			// Get CRTC to read the active mode
			DrmModeCrtc crtc;
			Memory::Zero(&crtc, sizeof(crtc));
			crtc.CrtcId = enc.CrtcId;

			if (Ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0)
				continue;

			if (!crtc.ModeValid || crtc.Mode.Hdisplay == 0 || crtc.Mode.Vdisplay == 0)
				continue;

			tempDevices[deviceCount].Left = -(INT32)(cardIdx + 1);
			tempDevices[deviceCount].Top = (INT32)crtc.CrtcId;
			tempDevices[deviceCount].Width = (UINT32)crtc.Mode.Hdisplay;
			tempDevices[deviceCount].Height = (UINT32)crtc.Mode.Vdisplay;
			tempDevices[deviceCount].Primary = (deviceCount == 0);
			deviceCount++;
		}

		System::Call(SYS_CLOSE, (USIZE)fd);
	}
}

/// @brief Capture screen via DRM dumb buffer mapping
/// @details Opens the DRM card, queries the CRTC framebuffer, maps it via
/// DRM_IOCTL_MODE_MAP_DUMB, and converts pixels to RGB.
/// @param device Display device with Left = -(cardIndex+1), Top = crtcId
/// @param buffer Output RGB pixel buffer
/// @return Ok on success, Err on failure
static Result<VOID, Error> DrmCapture(const ScreenDevice &device, Span<RGB> buffer)
{
	UINT32 cardIdx = (UINT32)(-(device.Left + 1));
	UINT32 crtcId = (UINT32)device.Top;

	SSIZE fd = OpenDrmCard(cardIdx);
	if (fd < 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Get CRTC to find the active framebuffer
	DrmModeCrtc crtc;
	Memory::Zero(&crtc, sizeof(crtc));
	crtc.CrtcId = crtcId;

	if (Ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || crtc.FbId == 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Get framebuffer info (returns GEM handle)
	DrmModeFbCmd fb;
	Memory::Zero(&fb, sizeof(fb));
	fb.FbId = crtc.FbId;

	if (Ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb) < 0 || fb.Handle == 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Validate dimensions
	if (fb.Width != device.Width || fb.Height != device.Height)
	{
		DrmGemClose gc;
		gc.Handle = fb.Handle;
		gc.Pad = 0;
		Ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Map the dumb buffer
	DrmModeMapDumb mapReq;
	Memory::Zero(&mapReq, sizeof(mapReq));
	mapReq.Handle = fb.Handle;

	if (Ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapReq) < 0)
	{
		DrmGemClose gc;
		gc.Handle = fb.Handle;
		gc.Pad = 0;
		Ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	USIZE mapSize = (USIZE)fb.Pitch * (USIZE)fb.Height;
	PVOID mapped = DrmMmapBuffer(mapSize, fd, mapReq.Offset);

	if (mapped == nullptr)
	{
		DrmGemClose gc;
		gc.Handle = fb.Handle;
		gc.Pad = 0;
		Ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Convert framebuffer pixels to RGB
	UINT8 *fbBase = (UINT8 *)mapped;
	PRGB rgbBuf = buffer.Data();
	UINT32 width = fb.Width;
	UINT32 height = fb.Height;
	UINT32 bytesPerPixel = fb.Bpp / 8;

	for (UINT32 y = 0; y < height; y++)
	{
		UINT8 *row = fbBase + (USIZE)y * fb.Pitch;

		for (UINT32 x = 0; x < width; x++)
		{
			UINT8 *src = row + (USIZE)x * bytesPerPixel;

			if (bytesPerPixel == 4)
			{
				// 32bpp XRGB8888: bytes are B, G, R, X (little-endian)
				rgbBuf[y * width + x].Red = src[2];
				rgbBuf[y * width + x].Green = src[1];
				rgbBuf[y * width + x].Blue = src[0];
			}
			else if (bytesPerPixel == 3)
			{
				// 24bpp BGR888
				rgbBuf[y * width + x].Red = src[2];
				rgbBuf[y * width + x].Green = src[1];
				rgbBuf[y * width + x].Blue = src[0];
			}
			else if (bytesPerPixel == 2)
			{
				// 16bpp RGB565
				UINT16 pixel = (UINT16)src[0] | ((UINT16)src[1] << 8);
				rgbBuf[y * width + x].Red = (UINT8)(((pixel >> 11) & 0x1F) * 255 / 31);
				rgbBuf[y * width + x].Green = (UINT8)(((pixel >> 5) & 0x3F) * 255 / 63);
				rgbBuf[y * width + x].Blue = (UINT8)((pixel & 0x1F) * 255 / 31);
			}
		}
	}

	// Cleanup: unmap, close GEM handle, close fd
	System::Call(SYS_MUNMAP, (USIZE)mapped, mapSize);

	DrmGemClose gc;
	gc.Handle = fb.Handle;
	gc.Pad = 0;
	Ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
	System::Call(SYS_CLOSE, (USIZE)fd);

	return Result<VOID, Error>::Ok();
}

// =============================================================================
// X11 raw protocol — screen capture via Unix domain socket
// =============================================================================
//
// Captures the composited desktop by speaking the X11 wire protocol directly
// over a Unix domain socket (/tmp/.X11-unix/X<N>). This works on X11 desktops
// with compositors (GNOME, KDE, XFCE, etc.) where DRM dumb buffers and
// /dev/fb* return black images because the GPU compositor owns the scanout
// buffers.
//
// Protocol flow:
// 1. Parse $DISPLAY to get display number
// 2. Connect to /tmp/.X11-unix/X<N> (AF_UNIX stream socket)
// 3. Read ~/.Xauthority for MIT-MAGIC-COOKIE-1 authentication
// 4. Perform X11 connection setup handshake
// 5. Send GetImage (opcode 73) on the root window in ZPixmap format
// 6. Read pixel data and convert to RGB using visual color masks
//
// @see X Window System Protocol, X Consortium Standard
//      https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html

#if defined(PLATFORM_LINUX)

/// @brief Unix domain socket address family (AF_UNIX = AF_LOCAL)
constexpr UINT16 X11_AF_UNIX = 1;

/// @brief Stream socket type for X11 (architecture-dependent)
#if defined(ARCHITECTURE_MIPS64)
constexpr INT32 X11_SOCK_STREAM = 2;
#else
constexpr INT32 X11_SOCK_STREAM = 1;
#endif

/// @brief X11 GetImage request opcode
/// @see X11 Protocol Section 8 — GetImage
constexpr UINT8 X11_OPCODE_GETIMAGE = 73;

/// @brief ZPixmap format for GetImage (pixels stored as full-depth values)
constexpr UINT8 X11_FORMAT_ZPIXMAP = 2;

/// @brief Xauthority family: local Unix connection
constexpr UINT16 XAUTH_FAMILY_LOCAL = 256;

/// @brief Xauthority family: wildcard (matches any host/display)
constexpr UINT16 XAUTH_FAMILY_WILD = 65535;

/// @brief Unix domain socket address (sockaddr_un equivalent)
struct SockAddrUn
{
	UINT16 SunFamily; ///< Address family (AF_UNIX = 1)
	CHAR SunPath[108]; ///< Socket pathname
};

#pragma pack(push, 1)

/// @brief X11 connection setup request (12 bytes, followed by auth data)
/// @see X11 Protocol Section 8 — Connection Setup
struct X11SetupRequest
{
	UINT8 ByteOrder;     ///< 0x6C = LSBFirst (little-endian), 0x42 = MSBFirst
	UINT8 Pad0;
	UINT16 MajorVersion; ///< Protocol major version (11)
	UINT16 MinorVersion; ///< Protocol minor version (0)
	UINT16 AuthNameLen;  ///< Length of authorization protocol name
	UINT16 AuthDataLen;  ///< Length of authorization protocol data
	UINT16 Pad1;
};

/// @brief X11 GetImage request (20 bytes)
/// @see X11 Protocol Section 8 — GetImage
struct X11GetImageRequest
{
	UINT8 Opcode;     ///< 73 (GetImage)
	UINT8 Format;     ///< 2 = ZPixmap
	UINT16 Length;    ///< Request length in 4-byte units (5)
	UINT32 Drawable;  ///< Root window ID
	INT16 X;          ///< Source X coordinate
	INT16 Y;          ///< Source Y coordinate
	UINT16 Width;     ///< Capture width in pixels
	UINT16 Height;    ///< Capture height in pixels
	UINT32 PlaneMask; ///< 0xFFFFFFFF for all bit planes
};

#pragma pack(pop)

/// @brief Parsed X11 connection info needed for screen capture
struct X11ConnectionInfo
{
	UINT32 RootWindow;  ///< Root window XID from connection setup
	UINT32 Width;       ///< Root window width in pixels
	UINT32 Height;      ///< Root window height in pixels
	UINT32 RootDepth;   ///< Root window depth (bits per pixel component)
	UINT32 RootVisual;  ///< Root window visual ID
	UINT32 Bpp;         ///< Bits per pixel for root depth (from pixmap formats)
	UINT32 RedMask;     ///< Red channel bitmask from visual
	UINT32 GreenMask;   ///< Green channel bitmask from visual
	UINT32 BlueMask;    ///< Blue channel bitmask from visual
};

// --- Unix domain socket helpers (mirror patterns from posix/socket.cc) ---

/// @brief Create a Unix domain stream socket
/// @return Socket fd on success, negative errno on failure
static SSIZE UnixSocket()
{
#if defined(ARCHITECTURE_I386)
	USIZE args[3] = {(USIZE)X11_AF_UNIX, (USIZE)X11_SOCK_STREAM, 0};
	return System::Call(SYS_SOCKETCALL, SOCKOP_SOCKET, (USIZE)args);
#else
	return System::Call(SYS_SOCKET, (USIZE)X11_AF_UNIX, (USIZE)X11_SOCK_STREAM, (USIZE)0);
#endif
}

/// @brief Connect a Unix domain socket to a server address
/// @param fd Socket file descriptor
/// @param addr Unix socket address
/// @param len Size of address structure
/// @return 0 on success, negative errno on failure
static SSIZE UnixDoConnect(SSIZE fd, const SockAddrUn *addr, UINT32 len)
{
#if defined(ARCHITECTURE_I386)
	USIZE args[3] = {(USIZE)fd, (USIZE)addr, (USIZE)len};
	return System::Call(SYS_SOCKETCALL, SOCKOP_CONNECT, (USIZE)args);
#else
	return System::Call(SYS_CONNECT, (USIZE)fd, (USIZE)addr, (USIZE)len);
#endif
}

/// @brief Send data on a connected socket
/// @param fd Socket file descriptor
/// @param buf Data buffer
/// @param len Number of bytes to send
/// @return Bytes sent on success, negative errno on failure
static SSIZE UnixSend(SSIZE fd, const VOID *buf, USIZE len)
{
#if defined(ARCHITECTURE_I386)
	USIZE args[4] = {(USIZE)fd, (USIZE)buf, len, 0};
	return System::Call(SYS_SOCKETCALL, SOCKOP_SEND, (USIZE)args);
#else
	return System::Call(SYS_SENDTO, (USIZE)fd, (USIZE)buf, len, 0, 0, 0);
#endif
}

/// @brief Receive data from a connected socket
/// @param fd Socket file descriptor
/// @param buf Output buffer
/// @param len Maximum bytes to receive
/// @return Bytes received on success, negative errno on failure
static SSIZE UnixRecv(SSIZE fd, VOID *buf, USIZE len)
{
#if defined(ARCHITECTURE_I386)
	USIZE args[4] = {(USIZE)fd, (USIZE)buf, len, 0};
	return System::Call(SYS_SOCKETCALL, SOCKOP_RECV, (USIZE)args);
#else
	return System::Call(SYS_RECVFROM, (USIZE)fd, (USIZE)buf, len, 0, 0, 0);
#endif
}

/// @brief Receive exactly len bytes, looping on partial reads
/// @param fd Socket file descriptor
/// @param buf Output buffer
/// @param len Exact number of bytes to receive
/// @return true if all bytes received, false on error or EOF
static BOOL UnixRecvAll(SSIZE fd, VOID *buf, USIZE len)
{
	UINT8 *p = (UINT8 *)buf;
	USIZE remaining = len;
	while (remaining > 0)
	{
		SSIZE n = UnixRecv(fd, p, remaining);
		if (n <= 0)
			return false;
		p += n;
		remaining -= (USIZE)n;
	}
	return true;
}

/// @brief Send all bytes, looping on partial writes
/// @param fd Socket file descriptor
/// @param buf Data buffer
/// @param len Exact number of bytes to send
/// @return true if all bytes sent, false on error
static BOOL UnixSendAll(SSIZE fd, const VOID *buf, USIZE len)
{
	const UINT8 *p = (const UINT8 *)buf;
	USIZE remaining = len;
	while (remaining > 0)
	{
		SSIZE n = UnixSend(fd, p, remaining);
		if (n <= 0)
			return false;
		p += n;
		remaining -= (USIZE)n;
	}
	return true;
}

/// @brief Open a file for reading (architecture-specific open/openat)
/// @param path Null-terminated file path
/// @return File descriptor on success, negative errno on failure
static SSIZE X11OpenFile(const CHAR *path)
{
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)O_RDONLY);
#else
	return System::Call(SYS_OPEN, (USIZE)path, (USIZE)O_RDONLY);
#endif
}

/// @brief Parse the DISPLAY environment variable to extract the display number
/// @details Handles formats ":N", ":N.S", and "localhost:N". Returns false for
/// remote displays (hostname other than "localhost") since those require TCP
/// forwarding, not a local Unix socket.
/// @param displayNum [out] Extracted display number
/// @return true if a valid local display was found and parsed
static BOOL X11ParseDisplay(UINT32 &displayNum)
{
	CHAR display[64];
	USIZE len = Environment::GetVariable("DISPLAY", Span<CHAR>(display, sizeof(display)));
	if (len == 0)
		return false;

	// Find the colon separator
	USIZE colonPos = 0;
	BOOL found = false;
	for (USIZE i = 0; i < len; i++)
	{
		if (display[i] == ':')
		{
			colonPos = i;
			found = true;
			break;
		}
	}
	if (!found)
		return false;

	// If text before colon, only accept empty or "localhost"
	if (colonPos > 0)
	{
		auto localhost = "localhost";
		if (colonPos != 9)
			return false;
		for (USIZE i = 0; i < 9; i++)
		{
			if (display[i] != ((const CHAR *)localhost)[i])
				return false;
		}
	}

	// Parse decimal display number after the colon
	displayNum = 0;
	for (USIZE i = colonPos + 1; i < len; i++)
	{
		if (display[i] == '.')
			break; // Screen number separator — ignore screen
		if (display[i] < '0' || display[i] > '9')
			return false;
		displayNum = displayNum * 10 + (UINT32)(display[i] - '0');
	}

	return true;
}

/// @brief Read a big-endian UINT16 from a byte pointer
/// @details Xauthority files use big-endian field encoding regardless of host
/// byte order.
static UINT16 ReadBE16(const UINT8 *p)
{
	return (UINT16)((UINT16)p[0] << 8) | (UINT16)p[1];
}

/// @brief Read MIT-MAGIC-COOKIE-1 auth data from the Xauthority file
/// @details Tries $XAUTHORITY first, then falls back to $HOME/.Xauthority.
/// Parses the binary Xauthority format to find an entry matching the display
/// number with MIT-MAGIC-COOKIE-1 authentication.
/// @param displayNum Display number to match
/// @param cookie [out] 16-byte buffer to receive the cookie
/// @return Cookie length (16 on success, 0 if not found or file unreadable)
/// @see xauth(1), Xsecurity(7)
static USIZE X11ReadAuth(UINT32 displayNum, UINT8 *cookie)
{
	// Determine Xauthority file path
	CHAR path[256];
	USIZE pathLen = Environment::GetVariable("XAUTHORITY", Span<CHAR>(path, sizeof(path)));

	if (pathLen == 0)
	{
		CHAR home[128];
		USIZE homeLen = Environment::GetVariable("HOME", Span<CHAR>(home, sizeof(home)));
		if (homeLen == 0)
			return 0;
		auto xauthSuffix = "/.Xauthority";
		Memory::Copy(path, home, homeLen);
		Memory::Copy(path + homeLen, (const CHAR *)xauthSuffix, 13);
		pathLen = homeLen + 12;
	}

	SSIZE fd = X11OpenFile(path);
	if (fd < 0)
		return 0;

	// Xauthority files are typically small (< 1KB)
	UINT8 authBuf[2048];
	SSIZE bytesRead = System::Call(SYS_READ, (USIZE)fd, (USIZE)authBuf, sizeof(authBuf));
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (bytesRead <= 0)
		return 0;

	// Convert display number to ASCII string for matching
	CHAR displayStr[12];
	USIZE displayStrLen = 0;
	{
		UINT32 num = displayNum;
		CHAR tmp[12];
		USIZE tmpLen = 0;
		if (num == 0)
		{
			tmp[tmpLen++] = '0';
		}
		else
		{
			while (num > 0)
			{
				tmp[tmpLen++] = '0' + (CHAR)(num % 10);
				num /= 10;
			}
		}
		for (USIZE i = 0; i < tmpLen; i++)
			displayStr[i] = tmp[tmpLen - 1 - i];
		displayStr[tmpLen] = '\0';
		displayStrLen = tmpLen;
	}

	// Parse Xauthority entries (binary format, multi-byte fields big-endian)
	auto mitCookieName = "MIT-MAGIC-COOKIE-1";
	const UINT8 *p = authBuf;
	const UINT8 *end = authBuf + bytesRead;

	while (p + 4 <= end)
	{
		UINT16 family = ReadBE16(p); p += 2;

		if (p + 2 > end) break;
		UINT16 addrLen = ReadBE16(p); p += 2;
		if (p + addrLen > end) break;
		p += addrLen; // skip address

		if (p + 2 > end) break;
		UINT16 numLen = ReadBE16(p); p += 2;
		if (p + numLen > end) break;
		const UINT8 *numStr = p;
		p += numLen;

		if (p + 2 > end) break;
		UINT16 nameLen = ReadBE16(p); p += 2;
		if (p + nameLen > end) break;
		const UINT8 *nameStr = p;
		p += nameLen;

		if (p + 2 > end) break;
		UINT16 dataLen = ReadBE16(p); p += 2;
		if (p + dataLen > end) break;
		const UINT8 *dataStr = p;
		p += dataLen;

		// Match: local/wild family, matching display number, MIT-MAGIC-COOKIE-1
		BOOL familyMatch = (family == XAUTH_FAMILY_LOCAL || family == XAUTH_FAMILY_WILD);
		BOOL displayMatch = false;

		if (family == XAUTH_FAMILY_WILD)
		{
			displayMatch = true;
		}
		else if (numLen == displayStrLen)
		{
			displayMatch = true;
			for (USIZE i = 0; i < displayStrLen; i++)
			{
				if (numStr[i] != (UINT8)displayStr[i])
				{
					displayMatch = false;
					break;
				}
			}
		}

		if (familyMatch && displayMatch && nameLen == 18 && dataLen == 16)
		{
			BOOL nameMatch = true;
			for (USIZE i = 0; i < 18; i++)
			{
				if (nameStr[i] != (UINT8)((const CHAR *)mitCookieName)[i])
				{
					nameMatch = false;
					break;
				}
			}
			if (nameMatch)
			{
				Memory::Copy(cookie, dataStr, 16);
				return 16;
			}
		}
	}

	return 0;
}

/// @brief Compute the shift amount and bit width from a color channel mask
/// @param mask Color channel bitmask (e.g. 0x00FF0000 for 8-bit red at bit 16)
/// @param shift [out] Number of bits to right-shift to reach the LSB
/// @param width [out] Number of contiguous set bits in the mask
static VOID X11ComputeMaskShift(UINT32 mask, UINT32 &shift, UINT32 &width)
{
	shift = 0;
	width = 0;
	if (mask == 0)
		return;
	UINT32 m = mask;
	while ((m & 1) == 0)
	{
		m >>= 1;
		shift++;
	}
	while (m & 1)
	{
		m >>= 1;
		width++;
	}
}

/// @brief Extract an 8-bit color component from a pixel value using a mask
/// @param pixel Raw pixel value
/// @param shift Bit position of the component (from X11ComputeMaskShift)
/// @param width Bit width of the component (from X11ComputeMaskShift)
/// @return Scaled 8-bit color value
static UINT8 X11ExtractChannel(UINT32 pixel, UINT32 shift, UINT32 width)
{
	if (width == 0)
		return 0;
	UINT32 value = (pixel >> shift) & ((1u << width) - 1);
	if (width < 8)
		value = (value * 255) / ((1u << width) - 1);
	else if (width > 8)
		value >>= (width - 8);
	return (UINT8)value;
}

/// @brief Open X11 connection, perform protocol handshake, return connection info
/// @details Connects to the X11 server via Unix domain socket, authenticates
/// using MIT-MAGIC-COOKIE-1 (or no auth), and parses the connection setup
/// response to extract root window ID, screen dimensions, and visual color
/// masks needed for GetImage pixel conversion.
/// @param displayNum X11 display number (from $DISPLAY)
/// @param info [out] Populated with root window, dimensions, visual info
/// @return Socket fd on success (caller must close), negative on failure
static SSIZE X11OpenConnection(UINT32 displayNum, X11ConnectionInfo &info)
{
	// Build socket path: /tmp/.X11-unix/X<N>
	auto socketBase = "/tmp/.X11-unix/X";
	CHAR socketPath[32];
	Memory::Copy(socketPath, (const CHAR *)socketBase, 17);

	USIZE pos = 16;
	if (displayNum == 0)
	{
		socketPath[pos++] = '0';
	}
	else
	{
		CHAR tmp[8];
		USIZE tmpLen = 0;
		UINT32 num = displayNum;
		while (num > 0)
		{
			tmp[tmpLen++] = '0' + (CHAR)(num % 10);
			num /= 10;
		}
		for (USIZE i = 0; i < tmpLen; i++)
			socketPath[pos++] = tmp[tmpLen - 1 - i];
	}
	socketPath[pos] = '\0';

	// Create Unix domain socket
	SSIZE fd = UnixSocket();
	if (fd < 0)
		return -1;

	// Connect to X11 server
	SockAddrUn addr;
	Memory::Zero(&addr, sizeof(addr));
	addr.SunFamily = X11_AF_UNIX;
	Memory::Copy(addr.SunPath, socketPath, pos + 1);

	if (UnixDoConnect(fd, &addr, sizeof(addr)) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	// Read Xauthority cookie for MIT-MAGIC-COOKIE-1 authentication
	UINT8 authCookie[16];
	USIZE cookieLen = X11ReadAuth(displayNum, authCookie);

	// Build connection setup request
	auto mitCookieName = "MIT-MAGIC-COOKIE-1";
	UINT16 authNameLen = (cookieLen > 0) ? 18 : 0;
	UINT16 authDataLen = (cookieLen > 0) ? 16 : 0;
	UINT16 authNamePad = (4 - (authNameLen % 4)) % 4;
	UINT16 authDataPad = (4 - (authDataLen % 4)) % 4;

	X11SetupRequest setupReq;
	setupReq.ByteOrder = 0x6C; // LSBFirst (little-endian)
	setupReq.Pad0 = 0;
	setupReq.MajorVersion = 11;
	setupReq.MinorVersion = 0;
	setupReq.AuthNameLen = authNameLen;
	setupReq.AuthDataLen = authDataLen;
	setupReq.Pad1 = 0;

	USIZE reqSize = sizeof(setupReq) + authNameLen + authNamePad + authDataLen + authDataPad;
	UINT8 reqBuf[64];
	Memory::Zero(reqBuf, sizeof(reqBuf));
	Memory::Copy(reqBuf, &setupReq, sizeof(setupReq));

	USIZE off = sizeof(setupReq);
	if (authNameLen > 0)
	{
		Memory::Copy(reqBuf + off, (const CHAR *)mitCookieName, 18);
		off += 18 + authNamePad;
		Memory::Copy(reqBuf + off, authCookie, 16);
		off += 16 + authDataPad;
	}

	if (!UnixSendAll(fd, reqBuf, reqSize))
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	// Read the 8-byte response prefix
	UINT8 prefix[8];
	if (!UnixRecvAll(fd, prefix, 8))
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	if (prefix[0] != 1) // Status: 0=Failed, 1=Success, 2=Authenticate
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	// Read the remaining setup data
	UINT16 additionalLen = *(UINT16 *)(prefix + 6); // 4-byte units
	USIZE dataLen = (USIZE)additionalLen * 4;
	if (dataLen > 8192 || dataLen < 32)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	UINT8 respBuf[8192];
	if (!UnixRecvAll(fd, respBuf, dataLen))
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	// Parse connection setup response:
	// Offset  0: release-number       (CARD32)
	// Offset  4: resource-id-base     (CARD32)
	// Offset  8: resource-id-mask     (CARD32)
	// Offset 12: motion-buffer-size   (CARD32)
	// Offset 16: vendor-length        (CARD16)
	// Offset 18: max-request-length   (CARD16)
	// Offset 20: number-of-screens    (CARD8)
	// Offset 21: number-of-formats    (CARD8)
	// Offset 22-27: byte-order, bitmap fields, keycodes
	// Offset 28: unused               (4 bytes)
	// Offset 32: vendor string (vendorLength, padded to 4)
	// Then: pixmap formats (numFormats * 8 bytes)
	// Then: screen structures
	UINT16 vendorLen = *(UINT16 *)(respBuf + 16);
	UINT8 numScreens = respBuf[20];
	UINT8 numFormats = respBuf[21];

	if (numScreens == 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	UINT16 vendorPad = (4 - (vendorLen % 4)) % 4;
	USIZE formatStart = 32 + vendorLen + vendorPad;
	USIZE screenStart = formatStart + (USIZE)numFormats * 8;

	if (screenStart + 40 > dataLen)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return -1;
	}

	// Parse first screen:
	// Offset  0: root window          (CARD32)
	// Offset  4: default-colormap     (CARD32)
	// Offset  8: white-pixel          (CARD32)
	// Offset 12: black-pixel          (CARD32)
	// Offset 16: current-input-masks  (CARD32)
	// Offset 20: width-in-pixels      (CARD16)
	// Offset 22: height-in-pixels     (CARD16)
	// Offset 24: width-in-mm          (CARD16)
	// Offset 26: height-in-mm         (CARD16)
	// Offset 28: min-installed-maps   (CARD16)
	// Offset 30: max-installed-maps   (CARD16)
	// Offset 32: root-visual          (CARD32)
	// Offset 36: backing-stores       (CARD8)
	// Offset 37: save-unders          (CARD8)
	// Offset 38: root-depth           (CARD8)
	// Offset 39: number-of-depths     (CARD8)
	const UINT8 *screen = respBuf + screenStart;
	info.RootWindow = *(UINT32 *)(screen + 0);
	info.Width = *(UINT16 *)(screen + 20);
	info.Height = *(UINT16 *)(screen + 22);
	info.RootVisual = *(UINT32 *)(screen + 32);
	info.RootDepth = screen[38];
	UINT8 numDepths = screen[39];

	// Find BPP from pixmap format matching root depth
	info.Bpp = 0;
	for (UINT8 i = 0; i < numFormats; i++)
	{
		const UINT8 *fmt = respBuf + formatStart + (USIZE)i * 8;
		if (fmt[0] == info.RootDepth)
		{
			info.Bpp = fmt[1];
			break;
		}
	}
	if (info.Bpp == 0)
		info.Bpp = 32; // Sensible default for modern displays

	// Walk depth/visual tree to find root visual's color masks
	info.RedMask = 0;
	info.GreenMask = 0;
	info.BlueMask = 0;

	const UINT8 *depthPtr = screen + 40;
	for (UINT8 d = 0; d < numDepths; d++)
	{
		if ((USIZE)(depthPtr - respBuf) + 8 > dataLen)
			break;

		// DEPTH: depth(1), unused(1), num-visuals(2), unused(4)
		UINT16 numVisuals = *(UINT16 *)(depthPtr + 2);
		const UINT8 *visualPtr = depthPtr + 8;

		for (UINT16 v = 0; v < numVisuals; v++)
		{
			if ((USIZE)(visualPtr - respBuf) + 24 > dataLen)
				break;

			// VISUALTYPE: visual-id(4), class(1), bits-per-rgb(1),
			//             colormap-entries(2), red-mask(4), green-mask(4),
			//             blue-mask(4), unused(4)
			UINT32 visualId = *(UINT32 *)(visualPtr + 0);
			if (visualId == info.RootVisual)
			{
				info.RedMask = *(UINT32 *)(visualPtr + 8);
				info.GreenMask = *(UINT32 *)(visualPtr + 12);
				info.BlueMask = *(UINT32 *)(visualPtr + 16);
			}
			visualPtr += 24;
		}

		depthPtr = visualPtr;
	}

	// Default to XRGB8888 if visual not found (common TrueColor layout)
	if (info.RedMask == 0 && info.GreenMask == 0 && info.BlueMask == 0)
	{
		info.RedMask = 0x00FF0000;
		info.GreenMask = 0x0000FF00;
		info.BlueMask = 0x000000FF;
	}

	return fd;
}

/// @brief Enumerate displays via X11 connection
/// @details Connects to the X11 server to read the root window dimensions.
/// X11 displays are encoded as Left = -(1000 + displayNum) to distinguish
/// from DRM devices (Left = -(1..8)) and framebuffer devices (Left = 0..7).
/// @param tempDevices Output array for discovered devices
/// @param deviceCount [in/out] Current device count, incremented per device
/// @param maxDevices Maximum capacity of tempDevices
static VOID X11GetDevices(ScreenDevice *tempDevices, UINT32 &deviceCount, UINT32 maxDevices)
{
	if (deviceCount >= maxDevices)
		return;

	UINT32 displayNum = 0;
	if (!X11ParseDisplay(displayNum))
		return;

	X11ConnectionInfo info;
	Memory::Zero(&info, sizeof(info));

	SSIZE fd = X11OpenConnection(displayNum, info);
	if (fd < 0)
		return;

	System::Call(SYS_CLOSE, (USIZE)fd);

	if (info.Width == 0 || info.Height == 0)
		return;

	tempDevices[deviceCount].Left = -(INT32)(1000 + displayNum);
	tempDevices[deviceCount].Top = 0;
	tempDevices[deviceCount].Width = info.Width;
	tempDevices[deviceCount].Height = info.Height;
	tempDevices[deviceCount].Primary = (deviceCount == 0);
	deviceCount++;
}

/// @brief Capture screen via X11 GetImage on root window
/// @details Opens an X11 connection, sends a GetImage request for the full
/// root window in ZPixmap format, reads the pixel data line by line, and
/// converts to RGB using the visual's color masks.
/// @param device Display device with Left = -(1000 + displayNum)
/// @param buffer Output RGB pixel buffer (must be device.Width * device.Height)
/// @return Ok on success, Err on connection/capture failure
static Result<VOID, Error> X11Capture(const ScreenDevice &device, Span<RGB> buffer)
{
	UINT32 displayNum = (UINT32)(-(device.Left + 1000));

	X11ConnectionInfo info;
	Memory::Zero(&info, sizeof(info));

	SSIZE fd = X11OpenConnection(displayNum, info);
	if (fd < 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Send GetImage request for the full root window
	X11GetImageRequest req;
	req.Opcode = X11_OPCODE_GETIMAGE;
	req.Format = X11_FORMAT_ZPIXMAP;
	req.Length = 5; // 20 bytes / 4
	req.Drawable = info.RootWindow;
	req.X = 0;
	req.Y = 0;
	req.Width = (UINT16)device.Width;
	req.Height = (UINT16)device.Height;
	req.PlaneMask = 0xFFFFFFFF;

	if (!UnixSendAll(fd, &req, sizeof(req)))
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Read 32-byte reply header
	// Reply format: type(1), depth(1), sequence(2), length(4),
	//               visual(4), unused(20)
	UINT8 replyHeader[32];
	if (!UnixRecvAll(fd, replyHeader, 32))
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Byte 0 must be 1 (Reply), not 0 (Error)
	if (replyHeader[0] != 1)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	UINT32 replyDataLen = *(UINT32 *)(replyHeader + 4); // 4-byte units
	USIZE totalDataBytes = (USIZE)replyDataLen * 4;

	// Compute scanline metrics
	UINT32 bytesPerPixel = info.Bpp / 8;
	if (bytesPerPixel == 0)
		bytesPerPixel = 4;
	UINT32 bytesPerLine = device.Width * bytesPerPixel;
	UINT32 linePad = (4 - (bytesPerLine % 4)) % 4;
	UINT32 paddedBytesPerLine = bytesPerLine + linePad;

	// Precompute color channel shift amounts from visual masks
	UINT32 redShift, redWidth, greenShift, greenWidth, blueShift, blueWidth;
	X11ComputeMaskShift(info.RedMask, redShift, redWidth);
	X11ComputeMaskShift(info.GreenMask, greenShift, greenWidth);
	X11ComputeMaskShift(info.BlueMask, blueShift, blueWidth);

	PRGB rgbBuf = buffer.Data();

	// Read and convert pixel data one scanline at a time
	// 32KB buffer supports up to 8192 pixels at 32bpp per scanline
	UINT8 lineBuf[32768];
	USIZE bytesConsumed = 0;

	for (UINT32 y = 0; y < device.Height; y++)
	{
		if (paddedBytesPerLine > sizeof(lineBuf))
		{
			System::Call(SYS_CLOSE, (USIZE)fd);
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		if (!UnixRecvAll(fd, lineBuf, paddedBytesPerLine))
		{
			System::Call(SYS_CLOSE, (USIZE)fd);
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}
		bytesConsumed += paddedBytesPerLine;

		for (UINT32 x = 0; x < device.Width; x++)
		{
			UINT8 *src = lineBuf + (USIZE)x * bytesPerPixel;
			UINT32 pixel = 0;

			// Assemble pixel value from little-endian bytes
			for (UINT32 b = 0; b < bytesPerPixel; b++)
				pixel |= (UINT32)src[b] << (b * 8);

			rgbBuf[y * device.Width + x].Red = X11ExtractChannel(pixel, redShift, redWidth);
			rgbBuf[y * device.Width + x].Green = X11ExtractChannel(pixel, greenShift, greenWidth);
			rgbBuf[y * device.Width + x].Blue = X11ExtractChannel(pixel, blueShift, blueWidth);
		}
	}

	// Drain any remaining reply padding
	while (bytesConsumed < totalDataBytes)
	{
		UINT8 discard[256];
		USIZE remaining = totalDataBytes - bytesConsumed;
		if (remaining > sizeof(discard))
			remaining = sizeof(discard);
		if (!UnixRecvAll(fd, discard, remaining))
			break;
		bytesConsumed += remaining;
	}

	System::Call(SYS_CLOSE, (USIZE)fd);
	return Result<VOID, Error>::Ok();
}

#endif // PLATFORM_LINUX

// =============================================================================
// Android screencap — screen capture via /system/bin/screencap
// =============================================================================
//
// Captures the display by forking /system/bin/screencap and reading its raw
// output from a pipe. This is the only reliable capture method on modern
// Android (10+) where /dev/fb*, /dev/graphics/fb*, and /dev/dri/card* are
// either absent or blocked by SELinux. The screencap tool uses SurfaceFlinger
// internally, providing access to the composited display without requiring
// Binder IPC from position-independent code.
//
// Raw output format (no flags):
//   UINT32 width
//   UINT32 height
//   UINT32 pixelFormat  (1=RGBA_8888, 2=RGBX_8888, 4=RGB_565, 5=BGRA_8888)
//   UINT8  pixelData[]  (width * height * bytesPerPixel)

#if defined(PLATFORM_ANDROID)

/// @brief Android screencap device encoding in ScreenDevice::Left
constexpr INT32 SCREENCAP_DEVICE_LEFT = -2000;

/// @brief Android pixel format constants (system/graphics.h HAL_PIXEL_FORMAT_*)
constexpr UINT32 ANDROID_PIXEL_FORMAT_RGBA_8888 = 1;
constexpr UINT32 ANDROID_PIXEL_FORMAT_RGBX_8888 = 2;
constexpr UINT32 ANDROID_PIXEL_FORMAT_RGB_565   = 4;
constexpr UINT32 ANDROID_PIXEL_FORMAT_BGRA_8888 = 5;

/// @brief Read exactly len bytes from a pipe, looping on partial reads
/// @param pipe Pipe to read from
/// @param buf Output buffer
/// @param len Exact number of bytes to read
/// @return true if all bytes read, false on error or EOF
static BOOL PipeReadAll(Pipe &pipe, UINT8 *buf, USIZE len)
{
	USIZE remaining = len;
	while (remaining > 0)
	{
		auto result = pipe.Read(Span<UINT8>(buf + (len - remaining), remaining));
		if (result.IsErr())
			return false;
		USIZE n = result.Value();
		if (n == 0)
			return false;
		remaining -= n;
	}
	return true;
}

/// @brief Find the screencap binary path
/// @return Path to screencap, or nullptr if not found
static const CHAR *FindScreencap()
{
	auto path1 = "/system/bin/screencap";
	auto path2 = "/vendor/bin/screencap";

	// Try to open to check existence (cheaper than Process::Create failure)
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	SSIZE fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)(const CHAR *)path1, (USIZE)O_RDONLY);
#else
	SSIZE fd = System::Call(SYS_OPEN, (USIZE)(const CHAR *)path1, (USIZE)O_RDONLY);
#endif
	if (fd >= 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return (const CHAR *)path1;
	}

#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)(const CHAR *)path2, (USIZE)O_RDONLY);
#else
	fd = System::Call(SYS_OPEN, (USIZE)(const CHAR *)path2, (USIZE)O_RDONLY);
#endif
	if (fd >= 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return (const CHAR *)path2;
	}

	return nullptr;
}

/// @brief Spawn screencap with optional -d display_id argument
/// @param screencapPath Path to screencap binary
/// @param displayId Display ID string (nullptr for no -d flag)
/// @param stdoutPipe Pipe whose write end is connected to screencap's stdout
/// @return Process on success, Err on failure
static Result<Process, Error> SpawnScreencap(const CHAR *screencapPath, const CHAR *displayId, Pipe &stdoutPipe)
{
	auto arg0 = "screencap";

	if (displayId != nullptr)
	{
		auto dashD = "-d";
		const CHAR *args[] = {(const CHAR *)arg0, (const CHAR *)dashD, displayId, nullptr};
		return Process::Create(screencapPath, args, -1, stdoutPipe.WriteEnd(), -1);
	}
	else
	{
		const CHAR *args[] = {(const CHAR *)arg0, nullptr};
		return Process::Create(screencapPath, args, -1, stdoutPipe.WriteEnd(), -1);
	}
}

/// @brief Run screencap and read the 16-byte header
/// @param screencapPath Path to screencap binary
/// @param displayId Display ID string (nullptr for default)
/// @param width [out] Display width
/// @param height [out] Display height
/// @return true on success
static BOOL ScreencapReadHeader(const CHAR *screencapPath, const CHAR *displayId,
								UINT32 &width, UINT32 &height)
{
	auto pipeResult = Pipe::Create();
	if (pipeResult.IsErr())
		return false;
	Pipe &stdoutPipe = pipeResult.Value();

	auto procResult = SpawnScreencap(screencapPath, displayId, stdoutPipe);
	if (procResult.IsErr())
		return false;
	Process &proc = procResult.Value();

	(VOID)stdoutPipe.CloseWrite();

	// Read 16 bytes — covers Android <9 (12-byte) and 9+ (16-byte) headers
	UINT8 header[16];
	BOOL ok = PipeReadAll(stdoutPipe, header, 16);

	(VOID)proc.Terminate();
	(VOID)proc.Wait();

	if (!ok)
		return false;

	width = *(UINT32 *)(header + 0);
	height = *(UINT32 *)(header + 4);

	return (width > 0 && height > 0 && width <= 16384 && height <= 16384);
}

/// @brief Get display IDs from dumpsys SurfaceFlinger
/// @details Runs "dumpsys SurfaceFlinger --display-id" and parses the output
/// to extract display ID strings. On multi-display devices (foldables),
/// screencap requires -d <display-id> to specify which display to capture.
/// @param ids [out] Array of display ID string buffers
/// @param maxIds Maximum number of IDs to extract
/// @return Number of display IDs found
static UINT32 GetAndroidDisplayIds(CHAR ids[][32], UINT32 maxIds)
{
	auto pipeResult = Pipe::Create();
	if (pipeResult.IsErr())
		return 0;
	Pipe &stdoutPipe = pipeResult.Value();

	auto dumpsysPath = "/system/bin/dumpsys";
	auto arg0 = "dumpsys";
	auto arg1 = "SurfaceFlinger";
	auto arg2 = "--display-id";
	const CHAR *args[] = {
		(const CHAR *)arg0,
		(const CHAR *)arg1,
		(const CHAR *)arg2,
		nullptr
	};

	auto procResult = Process::Create(
		(const CHAR *)dumpsysPath, args,
		-1, stdoutPipe.WriteEnd(), -1);

	if (procResult.IsErr())
		return 0;
	Process &proc = procResult.Value();

	(VOID)stdoutPipe.CloseWrite();

	// Read dumpsys output (typically small — a few display ID lines)
	CHAR buf[1024];
	USIZE totalRead = 0;
	while (totalRead < sizeof(buf) - 1)
	{
		auto result = stdoutPipe.Read(Span<UINT8>((UINT8 *)buf + totalRead, sizeof(buf) - 1 - totalRead));
		if (result.IsErr() || result.Value() == 0)
			break;
		totalRead += result.Value();
	}
	buf[totalRead] = '\0';

	(VOID)proc.Wait();

	// Parse output: each line with a long decimal number is a display ID
	// Example output:
	//   Display 0 (HWC display 0): port=0 pnpId=SEC ...
	//     id=4619827259835644929
	//   Display 2 (HWC display 1): port=1 pnpId=SEC ...
	//     id=4619827259835644930
	// Or just bare IDs, one per line
	UINT32 count = 0;
	USIZE i = 0;
	while (i < totalRead && count < maxIds)
	{
		// Skip whitespace/newlines
		while (i < totalRead && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r'))
			i++;

		if (i >= totalRead)
			break;

		// Check for "id=" prefix (Samsung/AOSP format)
		BOOL hasIdPrefix = false;
		if (i + 3 <= totalRead && buf[i] == 'i' && buf[i + 1] == 'd' && buf[i + 2] == '=')
		{
			i += 3;
			hasIdPrefix = true;
		}

		// Read a token (until whitespace/newline)
		USIZE tokenStart = i;
		while (i < totalRead && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r')
			i++;
		USIZE tokenLen = i - tokenStart;

		if (tokenLen == 0)
			continue;

		// Check if token is a number (display IDs are decimal numbers)
		BOOL isNumber = true;
		for (USIZE j = tokenStart; j < tokenStart + tokenLen; j++)
		{
			if (buf[j] < '0' || buf[j] > '9')
			{
				isNumber = false;
				break;
			}
		}

		// Accept: bare long numbers (>3 digits) or numbers after "id="
		if (isNumber && (hasIdPrefix || tokenLen > 3) && tokenLen < 31)
		{
			Memory::Copy(ids[count], buf + tokenStart, tokenLen);
			ids[count][tokenLen] = '\0';
			count++;
		}
	}

	return count;
}

/// @brief Enumerate displays via Android screencap
/// @details On multi-display devices (foldables like Samsung Z Fold),
/// screencap requires -d <display-id>. Enumerates display IDs via
/// "dumpsys SurfaceFlinger --display-id", then probes each with screencap.
/// Falls back to screencap without -d for single-display devices.
/// ScreenDevice::Left encodes -(2000 + displayIndex).
/// ScreenDevice::Top stores the display ID string hash for capture dispatch.
/// @param tempDevices Output array for discovered devices
/// @param deviceCount [in/out] Current device count
/// @param maxDevices Maximum capacity
static VOID ScreencapGetDevices(ScreenDevice *tempDevices, UINT32 &deviceCount, UINT32 maxDevices)
{
	if (deviceCount >= maxDevices)
		return;

	const CHAR *screencapPath = FindScreencap();
	if (screencapPath == nullptr)
		return;

	// Try to enumerate display IDs (needed for multi-display/foldable devices)
	CHAR displayIds[4][32];
	UINT32 displayCount = GetAndroidDisplayIds(displayIds, 4);

	if (displayCount > 0)
	{
		// Multi-display device: probe each display with screencap -d <id>
		for (UINT32 i = 0; i < displayCount && deviceCount < maxDevices; i++)
		{
			UINT32 width = 0, height = 0;
			if (!ScreencapReadHeader(screencapPath, displayIds[i], width, height))
				continue;

			tempDevices[deviceCount].Left = -(INT32)(2000 + i);
			tempDevices[deviceCount].Top = (INT32)i; // index into displayIds
			tempDevices[deviceCount].Width = width;
			tempDevices[deviceCount].Height = height;
			tempDevices[deviceCount].Primary = (deviceCount == 0);
			deviceCount++;
		}
	}

	// Fallback: try screencap without -d (single-display devices)
	if (deviceCount == 0)
	{
		UINT32 width = 0, height = 0;
		if (!ScreencapReadHeader(screencapPath, nullptr, width, height))
			return;

		tempDevices[deviceCount].Left = SCREENCAP_DEVICE_LEFT;
		tempDevices[deviceCount].Top = 0;
		tempDevices[deviceCount].Width = width;
		tempDevices[deviceCount].Height = height;
		tempDevices[deviceCount].Primary = true;
		deviceCount++;
	}
}

/// @brief Convert screencap pixel data to RGB
/// @param src Source pixel pointer
/// @param dst Destination RGB pixel
/// @param format Android pixel format constant
/// @param bytesPerPixel Bytes per source pixel
static VOID ScreencapPixelToRGB(const UINT8 *src, RGB &dst, UINT32 format)
{
	if (format == ANDROID_PIXEL_FORMAT_RGBA_8888 ||
		format == ANDROID_PIXEL_FORMAT_RGBX_8888)
	{
		dst.Red = src[0];
		dst.Green = src[1];
		dst.Blue = src[2];
	}
	else if (format == ANDROID_PIXEL_FORMAT_BGRA_8888)
	{
		dst.Red = src[2];
		dst.Green = src[1];
		dst.Blue = src[0];
	}
	else // RGB_565
	{
		UINT16 pixel = (UINT16)src[0] | ((UINT16)src[1] << 8);
		dst.Red = (UINT8)(((pixel >> 11) & 0x1F) * 255 / 31);
		dst.Green = (UINT8)(((pixel >> 5) & 0x3F) * 255 / 63);
		dst.Blue = (UINT8)((pixel & 0x1F) * 255 / 31);
	}
}

/// @brief Capture screen via Android screencap tool
/// @details Handles both single-display (Left = -2000) and multi-display
/// (Left = -(2000 + displayIndex)) devices. For multi-display, retrieves
/// display IDs via dumpsys and passes -d to screencap.
/// @param device Display device from ScreencapGetDevices
/// @param buffer Output RGB pixel buffer
/// @return Ok on success, Err on failure
static Result<VOID, Error> ScreencapCapture(const ScreenDevice &device, Span<RGB> buffer)
{
	const CHAR *screencapPath = FindScreencap();
	if (screencapPath == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Determine display ID for screencap -d
	const CHAR *displayId = nullptr;
	CHAR displayIdBuf[32];

	INT32 displayIndex = -(device.Left + 2000);
	if (displayIndex >= 0 && displayIndex < 4)
	{
		// Multi-display: re-enumerate to get the display ID string
		CHAR displayIds[4][32];
		UINT32 displayCount = GetAndroidDisplayIds(displayIds, 4);
		if ((UINT32)displayIndex < displayCount)
		{
			USIZE len = 0;
			while (displayIds[displayIndex][len] != '\0' && len < 31)
				len++;
			Memory::Copy(displayIdBuf, displayIds[displayIndex], len + 1);
			displayId = displayIdBuf;
		}
	}

	auto pipeResult = Pipe::Create();
	if (pipeResult.IsErr())
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	Pipe &stdoutPipe = pipeResult.Value();

	auto procResult = SpawnScreencap(screencapPath, displayId, stdoutPipe);
	if (procResult.IsErr())
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	Process &proc = procResult.Value();

	(VOID)stdoutPipe.CloseWrite();

	// Read 16-byte header (Android 9+ adds colorspace field at offset 12)
	UINT8 header[16];
	if (!PipeReadAll(stdoutPipe, header, 16))
	{
		(VOID)proc.Terminate();
		(VOID)proc.Wait();
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	UINT32 width = *(UINT32 *)(header + 0);
	UINT32 height = *(UINT32 *)(header + 4);
	UINT32 format = *(UINT32 *)(header + 8);

	// Detect 12-byte vs 16-byte header (Android 9+ colorspace: 0-2)
	UINT32 colorspace = *(UINT32 *)(header + 12);
	BOOL oldHeader = (colorspace > 2);
	UINT8 *extraBytes = oldHeader ? (header + 12) : nullptr;
	UINT32 extraLen = oldHeader ? 4 : 0;

	if (width != device.Width || height != device.Height)
	{
		(VOID)proc.Terminate();
		(VOID)proc.Wait();
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Determine bytes per pixel
	UINT32 bytesPerPixel;
	if (format == ANDROID_PIXEL_FORMAT_RGBA_8888 ||
		format == ANDROID_PIXEL_FORMAT_RGBX_8888 ||
		format == ANDROID_PIXEL_FORMAT_BGRA_8888)
	{
		bytesPerPixel = 4;
	}
	else if (format == ANDROID_PIXEL_FORMAT_RGB_565)
	{
		bytesPerPixel = 2;
	}
	else
	{
		(VOID)proc.Terminate();
		(VOID)proc.Wait();
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	PRGB rgbBuf = buffer.Data();
	UINT32 bytesPerLine = width * bytesPerPixel;
	UINT8 lineBuf[16384];

	for (UINT32 y = 0; y < height; y++)
	{
		if (bytesPerLine > sizeof(lineBuf))
		{
			(VOID)proc.Terminate();
			(VOID)proc.Wait();
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		// On pre-Android 9, we consumed 4 extra bytes from the header.
		if (extraLen > 0 && y == 0)
		{
			Memory::Copy(lineBuf, extraBytes, extraLen);
			if (!PipeReadAll(stdoutPipe, lineBuf + extraLen, bytesPerLine - extraLen))
			{
				(VOID)proc.Terminate();
				(VOID)proc.Wait();
				return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
			}
		}
		else if (!PipeReadAll(stdoutPipe, lineBuf, bytesPerLine))
		{
			(VOID)proc.Terminate();
			(VOID)proc.Wait();
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		for (UINT32 x = 0; x < width; x++)
		{
			UINT8 *src = lineBuf + (USIZE)x * bytesPerPixel;
			ScreencapPixelToRGB(src, rgbBuf[y * width + x], format);
		}
	}

	(VOID)proc.Wait();
	return Result<VOID, Error>::Ok();
}

#endif // PLATFORM_ANDROID

// =============================================================================
// Screen::GetDevices (X11 first, DRM second, framebuffer/screencap fallback)
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	constexpr UINT32 maxDevices = 8;
	ScreenDevice tempDevices[maxDevices];
	UINT32 deviceCount = 0;

#if defined(PLATFORM_LINUX)
	// Try X11 first (works on composited desktops where DRM/framebuffer fail)
	X11GetDevices(tempDevices, deviceCount, maxDevices);
#endif

	// Try DRM (/dev/dri/card*)
	if (deviceCount == 0)
		DrmGetDevices(tempDevices, deviceCount, maxDevices);

	// Fall back to framebuffer (/dev/fb*)
	if (deviceCount == 0)
	{
		for (UINT32 i = 0; i < maxDevices; i++)
		{
			SSIZE fd = OpenFramebuffer(i);
			if (fd < 0)
				continue;

			FbVarScreeninfo vinfo;
			Memory::Zero(&vinfo, sizeof(vinfo));

			SSIZE ret = Ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);
			System::Call(SYS_CLOSE, (USIZE)fd);

			if (ret < 0)
				continue;

			if (vinfo.Xres == 0 || vinfo.Yres == 0)
				continue;

			tempDevices[deviceCount].Left = (INT32)i;  // framebuffer index
			tempDevices[deviceCount].Top = 0;
			tempDevices[deviceCount].Width = vinfo.Xres;
			tempDevices[deviceCount].Height = vinfo.Yres;
			tempDevices[deviceCount].Primary = (deviceCount == 0);
			deviceCount++;
		}
	}

#if defined(PLATFORM_ANDROID)
	// Android last resort: use screencap tool (works on modern Android 10+
	// where /dev/fb* and /dev/dri/* are absent or blocked by SELinux)
	if (deviceCount == 0)
		ScreencapGetDevices(tempDevices, deviceCount, maxDevices);
#endif

	if (deviceCount == 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	ScreenDevice *devices = new ScreenDevice[deviceCount];
	if (devices == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_AllocFailed));

	Memory::Copy(devices, tempDevices, deviceCount * sizeof(ScreenDevice));

	ScreenDeviceList list;
	list.Devices = devices;
	list.Count = deviceCount;
	return Result<ScreenDeviceList, Error>::Ok(list);
}

// =============================================================================
// Internal helper — framebuffer capture by device index
// =============================================================================

/// @brief Capture screen via Linux framebuffer (/dev/fb<N>)
/// @param fbIndex Framebuffer device number (0-7)
/// @param device Display device descriptor (for resolution validation)
/// @param buffer Output RGB pixel buffer
/// @return Ok on success, Err on failure
static Result<VOID, Error> FbCapture(UINT32 fbIndex, const ScreenDevice &device, Span<RGB> buffer)
{
	if (fbIndex > 7)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	SSIZE fd = OpenFramebuffer(fbIndex);
	if (fd < 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Query screen parameters
	FbVarScreeninfo vinfo;
	Memory::Zero(&vinfo, sizeof(vinfo));

	FbFixScreeninfo finfo;
	Memory::Zero(&finfo, sizeof(finfo));

	if (Ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
		Ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Validate dimensions match the device
	if (vinfo.Xres != device.Width || vinfo.Yres != device.Height)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	UINT32 bytesPerPixel = vinfo.BitsPerPixel / 8;
	if (bytesPerPixel == 0 || bytesPerPixel > 4)
	{
		System::Call(SYS_CLOSE, (USIZE)fd);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Map the framebuffer memory
	PVOID mapped = MmapFramebuffer((USIZE)finfo.SmemLen, fd);
	System::Call(SYS_CLOSE, (USIZE)fd);

	if (mapped == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Calculate pointer to the visible area
	UINT8 *fbBase = (UINT8 *)mapped;
	USIZE visibleOffset = (USIZE)vinfo.Yoffset * finfo.LineLength +
						  (USIZE)vinfo.Xoffset * bytesPerPixel;

	PRGB rgbBuf = buffer.Data();
	UINT32 width = device.Width;
	UINT32 height = device.Height;

	// Convert framebuffer pixels to RGB
	for (UINT32 y = 0; y < height; y++)
	{
		UINT8 *row = fbBase + visibleOffset + (USIZE)y * finfo.LineLength;

		for (UINT32 x = 0; x < width; x++)
		{
			UINT32 pixel = 0;
			UINT8 *src = row + (USIZE)x * bytesPerPixel;

			// Read pixel bytes (little-endian)
			for (UINT32 b = 0; b < bytesPerPixel; b++)
				pixel |= (UINT32)src[b] << (b * 8);

			rgbBuf[y * width + x].Red = ExtractComponent(pixel, vinfo.Red);
			rgbBuf[y * width + x].Green = ExtractComponent(pixel, vinfo.Green);
			rgbBuf[y * width + x].Blue = ExtractComponent(pixel, vinfo.Blue);
		}
	}

	// Unmap framebuffer
	System::Call(SYS_MUNMAP, (USIZE)mapped, (USIZE)finfo.SmemLen);

	return Result<VOID, Error>::Ok();
}

/// @brief Try framebuffer capture across all /dev/fb devices matching resolution
/// @param device Display device descriptor (for resolution matching)
/// @param buffer Output RGB pixel buffer
/// @return Ok on success, Err if no matching framebuffer found
static Result<VOID, Error> FbCaptureFallback(const ScreenDevice &device, Span<RGB> buffer)
{
	for (UINT32 i = 0; i < 8; i++)
	{
		auto result = FbCapture(i, device, buffer);
		if (result.IsOk())
			return result;
	}
	return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
}

// =============================================================================
// Screen::Capture (X11, DRM, or framebuffer dispatch)
// =============================================================================

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer)
{
#if defined(PLATFORM_LINUX)
	// X11 device: Left <= -1000 encodes -(1000 + displayNum)
	if (device.Left <= -1000)
	{
		auto result = X11Capture(device, buffer);
		if (result.IsOk())
			return result;

		// X11 capture failed — fall through to DRM/framebuffer
	}
#endif

#if defined(PLATFORM_ANDROID)
	// Android screencap device: Left <= -2000 encodes -(2000 + displayIndex)
	if (device.Left <= SCREENCAP_DEVICE_LEFT)
		return ScreencapCapture(device, buffer);
#endif

	// DRM device: Left < 0 (and > -1000) encodes -(cardIndex + 1)
	if (device.Left < 0)
	{
		auto result = DrmCapture(device, buffer);
		if (result.IsOk())
		{
			// DRM capture can "succeed" but return all-black data when the
			// scanout buffer is GPU-allocated (not a dumb buffer). Sample
			// pixels to detect this and fall back to framebuffer.
			PRGB rgbBuf = buffer.Data();
			USIZE pixelCount = (USIZE)device.Width * (USIZE)device.Height;
			BOOL allBlack = true;

			// Sample up to 256 evenly spaced pixels
			USIZE step = pixelCount / 256;
			if (step == 0)
				step = 1;

			for (USIZE i = 0; i < pixelCount && allBlack; i += step)
			{
				if (rgbBuf[i].Red != 0 || rgbBuf[i].Green != 0 || rgbBuf[i].Blue != 0)
					allBlack = false;
			}

			if (!allBlack)
				return result;
		}

		// DRM capture failed or returned all-black — try framebuffer
		return FbCaptureFallback(device, buffer);
	}

	// Framebuffer device: Left encodes /dev/fb index
	return FbCapture((UINT32)device.Left, device, buffer);
}

#endif // platform selection
