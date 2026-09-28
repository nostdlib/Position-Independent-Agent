/**
 * @file screen.h
 * @brief Screen Device Enumeration and Capture
 *
 * @details Provides cross-platform screen device discovery and screenshot
 * capture. GetDevices() enumerates active displays with resolution and
 * position information. Capture() copies the framebuffer contents of a
 * specific display into an RGB pixel buffer; the region-hint overload
 * copies only the given regions, leaving the rest of the buffer at the
 * previous capture's content.
 *
 * @par Platform Implementations
 * - Windows: User32 EnumDisplayDevicesW/EnumDisplaySettingsW, GDI BitBlt/GetDIBits
 * - Linux/FreeBSD: DRM dumb buffers (/dev/dri/card*) with fbdev fallback (/dev/fb0..fb7)
 * - Android: screencap tool (preferred), DRM dumb buffers, fbdev (/dev/graphics/fb0..fb7)
 * - UEFI: Graphics Output Protocol (GOP) QueryMode/Blt
 * - Solaris: Framebuffer device (/dev/fb) with FBIOGTYPE ioctl + mmap
 * - macOS: CoreGraphics via dyld framework loader (dlopen/dlsym resolved from dyld Mach-O)
 * - iOS: Stub (requires UIKit/Objective-C runtime, not available in PIR context)
 *
 * @ingroup platform
 *
 * @defgroup display Display
 * @ingroup platform
 * @{
 */

#pragma once

#include "platform/platform.h"
#include "core/types/rgb.h"

#pragma pack(push, 1)
/// @brief Information about a connected display device
struct ScreenDevice
{
	INT32 Left;	   ///< X position on the virtual desktop
	INT32 Top;	   ///< Y position on the virtual desktop
	UINT32 Width;  ///< Horizontal resolution in pixels
	UINT32 Height; ///< Vertical resolution in pixels
	BOOL Primary;  ///< Whether this is the primary display
};
#pragma pack(pop)

/// @brief Result of Screen::GetDevices containing an array of display devices
struct ScreenDeviceList
{
	ScreenDevice *Devices; ///< Array of discovered display devices
	UINT32 Count;		   ///< Number of devices in the array

	/// @brief Free all allocated memory owned by this result
	VOID Free()
	{
		if (Devices)
		{
			delete[] Devices;
			Devices = nullptr;
		}
		
		Count = 0;
	}
};

/**
 * @brief Rectangular region of a display, in device pixels
 *
 * @details Platform-layer POD hint for partial captures: it mirrors the
 * beacon's dirty-rect shape without pulling lib/ types into platform
 * headers (layering). Regions are advisory — an implementation may
 * capture more than asked, up to the full frame.
 */
struct ScreenRegion
{
	UINT32 X;	  ///< Left edge in device coordinates
	UINT32 Y;	  ///< Top edge in device coordinates
	UINT32 Width;  ///< Region width in pixels
	UINT32 Height; ///< Region height in pixels
};

/**
 * @class Screen
 * @brief Screen device enumeration and framebuffer capture
 *
 * @details All methods are static. The class is stack-only
 * (no heap allocation of the class itself).
 */
class Screen
{
public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;

	/**
	 * @brief Enumerate all active display devices
	 *
	 * @details Discovers connected displays and returns their resolution,
	 * position on the virtual desktop, and primary status.
	 *
	 * @return Ok(ScreenDeviceList) on success (caller must call Free()),
	 *         Err on enumeration or allocation failure
	 */
	[[nodiscard]] static Result<ScreenDeviceList, Error> GetDevices();

	/**
	 * @brief Create opaque per-display state for fast repeated captures
	 *
	 * @details Allocates the heavyweight capture resources (Windows: compatible
	 * memory DC, bitmap, and conversion buffer) so per-frame Capture() calls
	 * reuse them instead of recreating and destroying everything. The state is
	 * bound to the device dimensions; Capture() rebuilds it on a mode change.
	 * Platforms without persistent resources return Ok(nullptr).
	 *
	 * @param device Display device the state is bound to (from GetDevices())
	 * @return Ok(state) on success, Err on allocation or initialization failure
	 */
	[[nodiscard]] static Result<PVOID, Error> CreateCaptureState(const ScreenDevice &device);

	/**
	 * @brief Release capture state created by CreateCaptureState
	 *
	 * @param state State to release; nullptr is a no-op
	 */
	static VOID DestroyCaptureState(PVOID state);

	/**
	 * @brief Capture a screenshot of the specified display device
	 *
	 * @details Copies the current framebuffer contents of the given display
	 * into the provided RGB buffer. The buffer must be at least
	 * device.Width * device.Height elements. Equivalent to
	 * Capture(device, buffer, captureState, nullptr, 0).
	 *
	 * @param device Display device to capture (from GetDevices())
	 * @param buffer Output RGB pixel buffer (top-down, left-to-right)
	 * @param captureState Optional state from CreateCaptureState() for repeated
	 *        captures of the same display; nullptr takes the stateless
	 *        create-per-call path
	 * @return Ok on success, Err on capture failure
	 */
	[[nodiscard]] static Result<VOID, Error> Capture(
		const ScreenDevice &device,
		Span<RGB> buffer,
		PVOID captureState = nullptr);

	/**
	 * @brief Capture only the given regions of the specified display device
	 *
	 * @details Partial capture for repeated captures through a persistent
	 * state: each region is copied at its native position and size, so the
	 * buffer holds a full frame whose un-captured regions keep whatever the
	 * previous capture left there — a downstream diff then reports no
	 * change in those regions. Platforms without per-region capture
	 * support capture the full frame and ignore the hints. The buffer must
	 * be at least device.Width * device.Height elements.
	 *
	 * @param device Display device to capture (from GetDevices())
	 * @param buffer Output RGB pixel buffer (top-down, left-to-right)
	 * @param captureState State from CreateCaptureState(); with nullptr the
	 *        hints are ignored and a full frame is captured
	 * @param regions Regions to copy (device pixel coordinates, within the
	 *        device bounds), or nullptr for a full capture
	 * @param regionCount Number of regions in regions
	 * @return Ok on success, Err on capture failure
	 */
	[[nodiscard]] static Result<VOID, Error> Capture(
		const ScreenDevice &device,
		Span<RGB> buffer,
		PVOID captureState,
		const ScreenRegion *regions,
		UINT32 regionCount);
};

/** @} */ // end of display group
