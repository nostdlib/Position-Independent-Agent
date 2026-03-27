/**
 * @file screen.cc
 * @brief macOS Screen Implementation via CoreGraphics
 *
 * @details Implements screen device enumeration and capture by dynamically
 * loading the CoreGraphics framework via the dyld resolver (dyld.cc).
 * This avoids linking against any system library — functions are resolved
 * at runtime by finding dlopen/dlsym in dyld's Mach-O symbol table.
 *
 * GetDevices() uses CGGetActiveDisplayList to enumerate online displays,
 * CGDisplayBounds for geometry, and CGDisplayIsMain for primary status.
 *
 * Capture() uses CGDisplayCreateImage to capture the display framebuffer,
 * CGImageGetDataProvider + CGDataProviderCopyData to extract raw pixel data,
 * and converts from the native BGRA/RGBA format to RGB.
 *
 * @see CoreGraphics Display Services
 *      https://developer.apple.com/documentation/coregraphics/quartz_display_services
 * @see CGDisplayCreateImage
 *      https://developer.apple.com/documentation/coregraphics/1454
 */

#include "platform/screen/screen.h"
#include "platform/kernel/macos/dyld.h"
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#include "core/memory/memory.h"

// =============================================================================
// CoreGraphics / CoreFoundation type aliases
// =============================================================================

/// @brief CGDirectDisplayID (uint32_t on macOS)
typedef UINT32 CGDisplayID;

/// @brief CGFloat is double on 64-bit macOS
typedef double CGFloat;

/// @brief CGRect — origin + size
struct CGRect
{
	CGFloat X;
	CGFloat Y;
	CGFloat Width;
	CGFloat Height;
};

/// @brief CGError values
constexpr INT32 kCGErrorSuccess = 0;

/// @brief CGBitmapInfo: byte order and alpha info
constexpr UINT32 kCGImageAlphaNoneSkipFirst = 6;       ///< xRGB (skip first byte)
constexpr UINT32 kCGBitmapByteOrder32Little = 2 << 12; ///< Little-endian 32-bit

// =============================================================================
// CoreGraphics function pointer types
// =============================================================================

typedef INT32 (*CGGetActiveDisplayListFn)(UINT32 maxDisplays, CGDisplayID *activeDisplays, UINT32 *displayCount);
typedef CGRect (*CGDisplayBoundsFn)(CGDisplayID display);
typedef UINT32 (*CGDisplayIsMainFn)(CGDisplayID display);
typedef PVOID (*CGDisplayCreateImageFn)(CGDisplayID display);
typedef USIZE (*CGImageGetWidthFn)(PVOID image);
typedef USIZE (*CGImageGetHeightFn)(PVOID image);
typedef USIZE (*CGImageGetBitsPerPixelFn)(PVOID image);
typedef USIZE (*CGImageGetBytesPerRowFn)(PVOID image);
typedef UINT32 (*CGImageGetBitmapInfoFn)(PVOID image);
typedef PVOID (*CGImageGetDataProviderFn)(PVOID image);
typedef PVOID (*CGDataProviderCopyDataFn)(PVOID provider);
typedef const UINT8 *(*CFDataGetBytePtrFn)(PVOID data);
typedef SSIZE (*CFDataGetLengthFn)(PVOID data);
typedef VOID (*CFReleaseFn)(PVOID cf);
typedef CGDisplayID (*CGMainDisplayIDFn)();

// =============================================================================
// Internal: Resolve CoreGraphics/CoreFoundation functions
// =============================================================================

/// @brief Resolved CoreGraphics function pointers
struct CGFunctions
{
	CGMainDisplayIDFn MainDisplayID;
	CGGetActiveDisplayListFn GetActiveDisplayList;
	CGDisplayBoundsFn DisplayBounds;
	CGDisplayIsMainFn DisplayIsMain;
	CGDisplayCreateImageFn DisplayCreateImage;
	CGImageGetWidthFn ImageGetWidth;
	CGImageGetHeightFn ImageGetHeight;
	CGImageGetBitsPerPixelFn ImageGetBitsPerPixel;
	CGImageGetBytesPerRowFn ImageGetBytesPerRow;
	CGImageGetBitmapInfoFn ImageGetBitmapInfo;
	CGImageGetDataProviderFn ImageGetDataProvider;
	CGDataProviderCopyDataFn DataProviderCopyData;
	CFDataGetBytePtrFn DataGetBytePtr;
	CFDataGetLengthFn DataGetLength;
	CFReleaseFn Release;
};

/// @brief Resolve a CoreGraphics function by name
/// @return Function pointer, or nullptr on failure
static PVOID ResolveCG(const CHAR *name)
{
	auto cgPath = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
	return ResolveFrameworkFunction((const CHAR *)cgPath, name);
}

/// @brief Resolve a CoreFoundation function by name
/// @return Function pointer, or nullptr on failure
static PVOID ResolveCF(const CHAR *name)
{
	auto cfPath = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
	return ResolveFrameworkFunction((const CHAR *)cfPath, name);
}

/// @brief Load all required CoreGraphics/CoreFoundation functions
/// @param[out] cg Output structure for resolved function pointers
/// @return true if all functions resolved successfully
static BOOL LoadCGFunctions(CGFunctions &cg)
{
	auto n0 = "CGMainDisplayID";
	auto n1 = "CGGetActiveDisplayList";
	auto n2 = "CGDisplayBounds";
	auto n3 = "CGDisplayIsMain";
	auto n4 = "CGDisplayCreateImage";
	auto n5 = "CGImageGetWidth";
	auto n6 = "CGImageGetHeight";
	auto n7 = "CGImageGetBitsPerPixel";
	auto n8 = "CGImageGetBytesPerRow";
	auto n9 = "CGImageGetBitmapInfo";
	auto n10 = "CGImageGetDataProvider";
	auto n11 = "CGDataProviderCopyData";
	auto n12 = "CFDataGetBytePtr";
	auto n13 = "CFDataGetLength";
	auto n14 = "CFRelease";

	cg.MainDisplayID = (CGMainDisplayIDFn)ResolveCG((const CHAR *)n0);
	cg.GetActiveDisplayList = (CGGetActiveDisplayListFn)ResolveCG((const CHAR *)n1);
	cg.DisplayBounds = (CGDisplayBoundsFn)ResolveCG((const CHAR *)n2);
	cg.DisplayIsMain = (CGDisplayIsMainFn)ResolveCG((const CHAR *)n3);
	cg.DisplayCreateImage = (CGDisplayCreateImageFn)ResolveCG((const CHAR *)n4);
	cg.ImageGetWidth = (CGImageGetWidthFn)ResolveCG((const CHAR *)n5);
	cg.ImageGetHeight = (CGImageGetHeightFn)ResolveCG((const CHAR *)n6);
	cg.ImageGetBitsPerPixel = (CGImageGetBitsPerPixelFn)ResolveCG((const CHAR *)n7);
	cg.ImageGetBytesPerRow = (CGImageGetBytesPerRowFn)ResolveCG((const CHAR *)n8);
	cg.ImageGetBitmapInfo = (CGImageGetBitmapInfoFn)ResolveCG((const CHAR *)n9);
	cg.ImageGetDataProvider = (CGImageGetDataProviderFn)ResolveCG((const CHAR *)n10);
	cg.DataProviderCopyData = (CGDataProviderCopyDataFn)ResolveCG((const CHAR *)n11);
	cg.DataGetBytePtr = (CFDataGetBytePtrFn)ResolveCF((const CHAR *)n12);
	cg.DataGetLength = (CFDataGetLengthFn)ResolveCF((const CHAR *)n13);
	cg.Release = (CFReleaseFn)ResolveCF((const CHAR *)n14);

	return cg.MainDisplayID != nullptr &&
		cg.GetActiveDisplayList != nullptr &&
		cg.DisplayBounds != nullptr &&
		cg.DisplayIsMain != nullptr &&
		cg.DisplayCreateImage != nullptr &&
		cg.ImageGetWidth != nullptr &&
		cg.ImageGetHeight != nullptr &&
		cg.ImageGetBitsPerPixel != nullptr &&
		cg.ImageGetBytesPerRow != nullptr &&
		cg.ImageGetBitmapInfo != nullptr &&
		cg.ImageGetDataProvider != nullptr &&
		cg.DataProviderCopyData != nullptr &&
		cg.DataGetBytePtr != nullptr &&
		cg.DataGetLength != nullptr &&
		cg.Release != nullptr;
}

// =============================================================================
// Internal: Fork wrapper and display availability probe
// =============================================================================

/// @brief Fork wrapper that correctly distinguishes parent/child on macOS.
/// @details macOS fork() returns child_pid in rax for BOTH parent and child.
/// The secondary return value (rdx on x86_64, x1 on aarch64) is 0 in the
/// parent and 1 in the child. System::Call clobbers this, so we use custom asm.
/// @return child_pid in parent, 0 in child, negative on error
static NOINLINE SSIZE MacFork()
{
#if defined(ARCHITECTURE_X86_64)
	register USIZE r_rax __asm__("rax") = SYS_FORK;
	register USIZE r_rdx __asm__("rdx");
	__asm__ volatile(
		"syscall\n"
		"jnc 1f\n"
		"negq %%rax\n"
		"1:\n"
		: "+r"(r_rax), "=r"(r_rdx)
		:
		: "rcx", "r11", "memory", "cc"
	);
	if ((SSIZE)r_rax < 0) return (SSIZE)r_rax;
	if (r_rdx != 0) return 0;
	return (SSIZE)r_rax;
#elif defined(ARCHITECTURE_AARCH64)
	register USIZE x0 __asm__("x0");
	register USIZE x1 __asm__("x1");
	register USIZE x16 __asm__("x16") = SYS_FORK;
	__asm__ volatile(
		"svc #0x80\n"
		"b.cc 1f\n"
		"neg x0, x0\n"
		"1:\n"
		: "=r"(x0), "=r"(x1)
		: "r"(x16)
		: "memory", "cc"
	);
	if ((SSIZE)x0 < 0) return (SSIZE)x0;
	if (x1 != 0) return 0;
	return (SSIZE)x0;
#endif
}

/// @brief Probe if CoreGraphics display services are usable.
/// @details Forks a child process that attempts to load CoreGraphics and
/// query the main display. If the child crashes (SIGKILL from TCC or
/// SIGSYS from blocked syscalls on headless CI runners), the parent
/// survives and returns false. This avoids crashing the main process.
/// @return true if display services are available, false otherwise
static BOOL ProbeDisplayAvailable()
{
	SSIZE pid = MacFork();
	if (pid < 0)
		return false;

	if (pid == 0)
	{
		// Child: attempt CG operations that may crash on headless systems
		CGFunctions cg;
		Memory::Zero(&cg, sizeof(cg));
		BOOL ok = LoadCGFunctions(cg);
		if (ok)
			ok = (cg.MainDisplayID() != 0);
		System::Call(SYS_EXIT, (USIZE)(ok ? 0 : 1));
		for (;;) {} // unreachable
	}

	// Parent: wait for child and check exit status
	INT32 status = 0;
	System::Call(SYS_WAIT4, (USIZE)pid, (USIZE)&status, (USIZE)0, (USIZE)0);

	// WIFEXITED(status) && WEXITSTATUS(status) == 0
	return (status & 0x7F) == 0 && ((status >> 8) & 0xFF) == 0;
}

// =============================================================================
// Screen::GetDevices
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	// Probe display availability in a forked child process.
	// CoreGraphics functions may crash (SIGKILL/SIGSYS) on headless CI
	// runners or sandboxed environments. The fork isolates the crash.
	if (!ProbeDisplayAvailable())
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	CGFunctions cg;
	Memory::Zero(&cg, sizeof(cg));

	if (!LoadCGFunctions(cg))
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	// Enumerate active displays (up to 16)
	constexpr UINT32 maxDisplays = 16;
	CGDisplayID displayIDs[maxDisplays];
	UINT32 displayCount = 0;

	INT32 err = cg.GetActiveDisplayList(maxDisplays, displayIDs, &displayCount);
	if (err != kCGErrorSuccess || displayCount == 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	ScreenDevice *devices = new ScreenDevice[displayCount];
	if (devices == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_AllocFailed));

	for (UINT32 i = 0; i < displayCount; i++)
	{
		CGRect bounds = cg.DisplayBounds(displayIDs[i]);

		devices[i].Left = (INT32)bounds.X;
		devices[i].Top = (INT32)bounds.Y;
		devices[i].Width = (UINT32)bounds.Width;
		devices[i].Height = (UINT32)bounds.Height;
		devices[i].Primary = (cg.DisplayIsMain(displayIDs[i]) != 0);
	}

	ScreenDeviceList list;
	list.Devices = devices;
	list.Count = displayCount;
	return Result<ScreenDeviceList, Error>::Ok(list);
}

// =============================================================================
// Screen::Capture
// =============================================================================

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer)
{
	if (!ProbeDisplayAvailable())
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	CGFunctions cg;
	Memory::Zero(&cg, sizeof(cg));

	if (!LoadCGFunctions(cg))
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Find the matching display ID
	constexpr UINT32 maxDisplays = 16;
	CGDisplayID displayIDs[maxDisplays];
	UINT32 displayCount = 0;

	INT32 err = cg.GetActiveDisplayList(maxDisplays, displayIDs, &displayCount);
	if (err != kCGErrorSuccess || displayCount == 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Match by position and dimensions
	CGDisplayID targetDisplay = 0;
	BOOL found = false;

	for (UINT32 i = 0; i < displayCount; i++)
	{
		CGRect bounds = cg.DisplayBounds(displayIDs[i]);

		if ((INT32)bounds.X == device.Left &&
			(INT32)bounds.Y == device.Top &&
			(UINT32)bounds.Width == device.Width &&
			(UINT32)bounds.Height == device.Height)
		{
			targetDisplay = displayIDs[i];
			found = true;
			break;
		}
	}

	if (!found)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Capture the display
	PVOID image = cg.DisplayCreateImage(targetDisplay);
	if (image == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	USIZE imgWidth = cg.ImageGetWidth(image);
	USIZE imgHeight = cg.ImageGetHeight(image);
	USIZE bitsPerPixel = cg.ImageGetBitsPerPixel(image);
	USIZE bytesPerRow = cg.ImageGetBytesPerRow(image);
	UINT32 bitmapInfo = cg.ImageGetBitmapInfo(image);

	if (imgWidth == 0 || imgHeight == 0 || bitsPerPixel < 24)
	{
		cg.Release(image);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Get raw pixel data via data provider
	PVOID provider = cg.ImageGetDataProvider(image);
	if (provider == nullptr)
	{
		cg.Release(image);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	PVOID cfData = cg.DataProviderCopyData(provider);
	if (cfData == nullptr)
	{
		cg.Release(image);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	const UINT8 *pixelData = cg.DataGetBytePtr(cfData);
	if (pixelData == nullptr)
	{
		cg.Release(cfData);
		cg.Release(image);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Determine pixel layout from bitmap info
	// Common macOS formats:
	//   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little = 0x2006 → BGRA (B at byte 0)
	//   kCGImageAlphaNoneSkipLast  | kCGBitmapByteOrder32Little = 0x2005 → BGRX
	//   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big    = 0x4006 → xRGB (R at byte 1)
	UINT32 alphaInfo = bitmapInfo & 0x1F;
	UINT32 byteOrder = bitmapInfo & 0xF000;
	UINT32 bytesPerPixel = (UINT32)(bitsPerPixel / 8);

	// Determine RGB byte offsets within each pixel
	UINT32 rOff, gOff, bOff;

	if (byteOrder == kCGBitmapByteOrder32Little)
	{
		// Little-endian 32-bit: memory layout is BGRA or BGRX
		bOff = 0;
		gOff = 1;
		rOff = 2;
	}
	else if (alphaInfo == kCGImageAlphaNoneSkipFirst)
	{
		// Big-endian xRGB: skip first byte
		rOff = 1;
		gOff = 2;
		bOff = 3;
	}
	else
	{
		// Default: assume RGBA
		rOff = 0;
		gOff = 1;
		bOff = 2;
	}

	// Convert to RGB
	PRGB rgbBuf = buffer.Data();
	UINT32 width = (UINT32)imgWidth;
	UINT32 height = (UINT32)imgHeight;

	// Clamp to device dimensions (Retina displays may return 2x resolution)
	if (width > device.Width)
		width = device.Width;
	if (height > device.Height)
		height = device.Height;

	for (UINT32 y = 0; y < height; y++)
	{
		const UINT8 *row = pixelData + y * bytesPerRow;

		for (UINT32 x = 0; x < width; x++)
		{
			const UINT8 *px = row + (USIZE)x * bytesPerPixel;
			rgbBuf[y * device.Width + x].Red = px[rOff];
			rgbBuf[y * device.Width + x].Green = px[gOff];
			rgbBuf[y * device.Width + x].Blue = px[bOff];
		}
	}

	cg.Release(cfData);
	cg.Release(image);

	return Result<VOID, Error>::Ok();
}
