/**
 * @file screen.h
 * @brief Screen Device Enumeration and Capture
 *
 * @details Provides cross-platform screen device discovery and screenshot
 * capture. GetDevices() enumerates active displays with resolution and
 * position information. Capture() copies the framebuffer contents of a
 * specific display into an RGB pixel buffer.
 *
 * @par Platform Implementations
 * - Windows: User32 EnumDisplayDevicesW/EnumDisplaySettingsW, GDI BitBlt/GetDIBits
 * - Linux/Android/FreeBSD: DRM dumb buffers (/dev/dri/card*) with fbdev fallback (/dev/fb0..fb7)
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
	 * @brief Capture a screenshot of the specified display device
	 *
	 * @details Copies the current framebuffer contents of the given display
	 * into the provided RGB buffer. The buffer must be at least
	 * device.Width * device.Height elements.
	 *
	 * @param device Display device to capture (from GetDevices())
	 * @param buffer Output RGB pixel buffer (top-down, left-to-right)
	 * @return Ok on success, Err on capture failure
	 */
	[[nodiscard]] static Result<VOID, Error> Capture(
		const ScreenDevice &device,
		Span<RGB> buffer);
};

/** @} */ // end of display group
