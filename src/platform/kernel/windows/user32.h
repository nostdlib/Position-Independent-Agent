/**
 * @file user32.h
 * @brief User32.dll Win32 API Wrappers
 *
 * @details Provides position-independent wrappers around the Windows Win32 API
 * functions exported by user32.dll. These wrappers provide display enumeration
 * and device context management operations.
 *
 * All function addresses are resolved dynamically at runtime via
 * ResolveUser32ExportAddress() using DJB2 hash-based PEB module lookup,
 * eliminating static import table entries.
 *
 * @note User32.dll may not be loaded by default in console applications.
 * Use NTDLL::LdrLoadDll to ensure it is loaded before calling these wrappers.
 *
 * @see Windows API Index
 *      https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-api-list
 */

#pragma once

#include "core/types/primitives.h"
#include "platform/kernel/windows/windows_types.h"
#include "core/algorithms/djb2.h"
#include "core/types/error.h"
#include "core/types/result.h"

#define DISPLAY_DEVICE_ACTIVE         0x00000001
#define DISPLAY_DEVICE_PRIMARY_DEVICE 0x00000004
#define ENUM_CURRENT_SETTINGS         ((UINT32)-1)

/**
 * @brief Display device information structure.
 *
 * @details Populated by EnumDisplayDevicesW with the device name, description
 * string, state flags, and device identifiers. StateFlags indicates whether
 * the device is active and/or the primary display.
 *
 * @see DISPLAY_DEVICEW structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-display_devicew
 */
typedef struct _DISPLAY_DEVICEW
{
	UINT32 cb;               ///< Size of this structure in bytes (must be set before calling)
	WCHAR DeviceName[32];    ///< Device name (e.g., "\\\\.\\DISPLAY1")
	WCHAR DeviceString[128]; ///< Device description string
	UINT32 StateFlags;       ///< Device state flags (DISPLAY_DEVICE_ACTIVE, DISPLAY_DEVICE_PRIMARY_DEVICE)
	WCHAR DeviceID[128];     ///< Device interface ID
	WCHAR DeviceKey[128];    ///< Registry key path for the device
} DISPLAY_DEVICEW, *PDISPLAY_DEVICEW;

/**
 * @brief Display settings structure (display-mode subset of DEVMODEW).
 *
 * @details Contains display resolution, position, orientation, and refresh
 * rate information. Populated by EnumDisplaySettingsW. The dmSize field must
 * be set to sizeof(DEVMODEW) before calling.
 *
 * @see DEVMODEW structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-devmodew
 */
typedef struct _DEVMODEW
{
	WCHAR dmDeviceName[32];        ///< 0: Device name (64 bytes)
	UINT16 dmSpecVersion;          ///< 64: Specification version
	UINT16 dmDriverVersion;        ///< 66: Driver version
	UINT16 dmSize;                 ///< 68: Size of this structure in bytes
	UINT16 dmDriverExtra;          ///< 70: Size of private driver data following this structure
	UINT32 dmFields;               ///< 72: Bitmask indicating which fields are valid
	INT32 dmPositionX;             ///< 76: Display X position (POINTL.x)
	INT32 dmPositionY;             ///< 80: Display Y position (POINTL.y)
	UINT32 dmDisplayOrientation;   ///< 84: Display rotation (DMDO_DEFAULT, etc.)
	UINT32 dmDisplayFixedOutput;   ///< 88: Fixed output mode
	INT16 dmColor;                 ///< 92: Color mode
	INT16 dmDuplex;                ///< 94: Duplex mode
	INT16 dmYResolution;           ///< 96: Y resolution
	INT16 dmTTOption;              ///< 98: TrueType option
	INT16 dmCollate;               ///< 100: Collation
	WCHAR dmFormName[32];          ///< 102: Form name (64 bytes)
	UINT16 dmLogPixels;            ///< 166: Logical pixels per inch
	UINT32 dmBitsPerPel;           ///< 168: Bits per pixel
	UINT32 dmPelsWidth;            ///< 172: Display width in pixels
	UINT32 dmPelsHeight;           ///< 176: Display height in pixels
	UINT32 dmDisplayFlags;         ///< 180: Display flags
	UINT32 dmDisplayFrequency;     ///< 184: Refresh rate in Hz
	UINT32 dmICMMethod;            ///< 188
	UINT32 dmICMIntent;            ///< 192
	UINT32 dmMediaType;            ///< 196
	UINT32 dmDitherType;           ///< 200
	UINT32 dmReserved1;            ///< 204
	UINT32 dmReserved2;            ///< 208
	UINT32 dmPanningWidth;         ///< 212
	UINT32 dmPanningHeight;        ///< 216
} DEVMODEW, *PDEVMODEW; // sizeof = 220

/**
 * @brief Wrappers for Win32 API functions exported by user32.dll.
 *
 * @details Provides position-independent access to user32.dll exports for
 * display enumeration and device context management. All function addresses
 * are resolved dynamically via ResolveUser32ExportAddress() at call time.
 *
 * @see Windows API Index
 *      https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-api-list
 */
class User32
{
public:
	/**
	 * @brief Enumerates display devices attached to the system.
	 *
	 * @details Retrieves information about a display device. Call repeatedly
	 * with incrementing iDevNum (starting from 0) until it returns false.
	 *
	 * @param lpDevice Device name to enumerate monitors for, or NULL for all adapters.
	 * @param iDevNum Zero-based index of the display device.
	 * @param lpDisplayDevice Pointer to DISPLAY_DEVICEW to receive device info (cb must be set).
	 * @param dwFlags Reserved; set to 0.
	 *
	 * @return true if device exists at index, false if no more devices.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see EnumDisplayDevicesW
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw
	 */
	[[nodiscard]] static BOOL EnumDisplayDevicesW(const WCHAR *lpDevice, UINT32 iDevNum, PDISPLAY_DEVICEW lpDisplayDevice, UINT32 dwFlags);

	/**
	 * @brief Retrieves display settings for a display device.
	 *
	 * @details Queries the current or registry-stored display settings for
	 * the specified device. Use ENUM_CURRENT_SETTINGS for the active mode.
	 *
	 * @param lpszDeviceName Device name from DISPLAY_DEVICEW::DeviceName, or NULL for primary.
	 * @param iModeNum Display mode index, or ENUM_CURRENT_SETTINGS for current mode.
	 * @param lpDevMode Pointer to DEVMODEW to receive settings (dmSize must be set).
	 *
	 * @return true if successful, false on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see EnumDisplaySettingsW
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw
	 */
	[[nodiscard]] static BOOL EnumDisplaySettingsW(const WCHAR *lpszDeviceName, UINT32 iModeNum, PDEVMODEW lpDevMode);

	/**
	 * @brief Retrieves a handle to the device context for the entire screen.
	 *
	 * @details When hWnd is NULL, returns a DC for the entire virtual screen.
	 * The returned DC must be released with ReleaseDC when no longer needed.
	 *
	 * @param hWnd Handle to the window, or NULL for the entire screen.
	 *
	 * @return Device context handle, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see GetDC
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdc
	 */
	[[nodiscard]] static PVOID GetDC(PVOID hWnd);

	/**
	 * @brief Releases a device context obtained by GetDC.
	 *
	 * @param hWnd Handle to the window whose DC is being released, or NULL.
	 * @param hDC Handle to the device context to release.
	 *
	 * @return Nonzero if released, zero on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ReleaseDC
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-releasedc
	 */
	static INT32 ReleaseDC(PVOID hWnd, PVOID hDC);
};
