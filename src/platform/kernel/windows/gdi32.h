/**
 * @file gdi32.h
 * @brief GDI32.dll Win32 API Wrappers
 *
 * @details Provides position-independent wrappers around the Windows GDI
 * functions exported by gdi32.dll. These wrappers provide bitmap creation,
 * device context management, and pixel data extraction operations.
 *
 * All function addresses are resolved dynamically at runtime via
 * ResolveGdi32ExportAddress() using DJB2 hash-based PEB module lookup,
 * eliminating static import table entries.
 *
 * @note Gdi32.dll may not be loaded by default in console applications.
 * Use NTDLL::LdrLoadDll to ensure it is loaded before calling these wrappers.
 *
 * @see Windows GDI
 *      https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi
 */

#pragma once

#include "core/types/primitives.h"
#include "platform/kernel/windows/windows_types.h"
#include "core/algorithms/djb2.h"
#include "core/types/error.h"
#include "core/types/result.h"

#define SRCCOPY        0x00CC0020
#define BI_RGB         0
#define DIB_RGB_COLORS 0

/**
 * @brief Bitmap information header defining the dimensions and color format.
 *
 * @details Used with GetDIBits to specify the desired output format and
 * receive bitmap dimension information.
 *
 * @see BITMAPINFOHEADER structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader
 */
typedef struct _BITMAPINFOHEADER
{
	UINT32 biSize;          ///< Size of this structure in bytes
	INT32 biWidth;          ///< Bitmap width in pixels
	INT32 biHeight;         ///< Bitmap height in pixels (negative = top-down)
	UINT16 biPlanes;        ///< Number of color planes (must be 1)
	UINT16 biBitCount;      ///< Bits per pixel (1, 4, 8, 16, 24, or 32)
	UINT32 biCompression;   ///< Compression type (BI_RGB = uncompressed)
	UINT32 biSizeImage;     ///< Image size in bytes (may be 0 for BI_RGB)
	INT32 biXPelsPerMeter;  ///< Horizontal resolution in pixels per meter
	INT32 biYPelsPerMeter;  ///< Vertical resolution in pixels per meter
	UINT32 biClrUsed;       ///< Number of color indices in the color table
	UINT32 biClrImportant;  ///< Number of important color indices
} BITMAPINFOHEADER, *PBITMAPINFOHEADER;

/**
 * @brief RGB color quad (color-table element of a BITMAPINFO).
 *
 * @see RGBQUAD structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-rgbquad
 */
typedef struct _RGBQUAD
{
	UINT8 rgbBlue;     ///< Blue intensity
	UINT8 rgbGreen;    ///< Green intensity
	UINT8 rgbRed;      ///< Red intensity
	UINT8 rgbReserved; ///< Reserved (must be zero)
} RGBQUAD;

/**
 * @brief Bitmap information: header plus the first color-table entry.
 *
 * @details 32bpp BI_RGB bitmaps carry no color table, so a bare
 * BITMAPINFOHEADER is a valid BITMAPINFO for them.
 *
 * @see BITMAPINFO structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfo
 */
typedef struct _BITMAPINFO
{
	BITMAPINFOHEADER bmiHeader; ///< Bitmap dimensions and color format
	RGBQUAD bmiColors[1];       ///< First color-table entry
} BITMAPINFO, *PBITMAPINFO;

/**
 * @brief Wrappers for Win32 GDI functions exported by gdi32.dll.
 *
 * @details Provides position-independent access to gdi32.dll exports for
 * bitmap creation, device context operations, and pixel data extraction.
 * All function addresses are resolved dynamically via
 * ResolveGdi32ExportAddress() at call time.
 *
 * @see Windows GDI
 *      https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi
 */
class Gdi32
{
public:
	/**
	 * @brief Creates a memory device context compatible with the specified DC.
	 *
	 * @param hdc Handle to an existing DC, or NULL for the screen DC.
	 * @return Handle to the memory DC, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see CreateCompatibleDC
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createcompatibledc
	 */
	[[nodiscard]] static PVOID CreateCompatibleDC(PVOID hdc);

	/**
	 * @brief Creates a device context for a named display device.
	 *
	 * @details A per-monitor DC created with the device name from
	 * EnumDisplayDevicesW sources exactly that monitor (origin at its
	 * top-left), reaching displays the shared GetDC(nullptr) DC cannot
	 * (hybrid graphics, DPI-virtualized sessions). Private DCs are meant
	 * to be held long-term and released with DeleteDC.
	 *
	 * @param lpszDriver Driver name (L"DISPLAY" for display devices).
	 * @param lpszDevice Device name (e.g. L"\\\\.\\DISPLAY2"), or NULL for the primary.
	 * @param lpszOutput Port name (unused; pass NULL).
	 * @param lpInitData Optional DEVMODEW (pass NULL for current settings).
	 * @return Handle to the DC, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see CreateDCW
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdcw
	 */
	[[nodiscard]] static PVOID CreateDCW(PCWCHAR lpszDriver, PCWCHAR lpszDevice, PCWCHAR lpszOutput, PCVOID lpInitData);

	/**
	 * @brief Creates a bitmap compatible with the specified device context.
	 *
	 * @param hdc Handle to a device context.
	 * @param cx Bitmap width in pixels.
	 * @param cy Bitmap height in pixels.
	 * @return Handle to the bitmap, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see CreateCompatibleBitmap
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createcompatiblebitmap
	 */
	[[nodiscard]] static PVOID CreateCompatibleBitmap(PVOID hdc, INT32 cx, INT32 cy);

	/**
	 * @brief Creates a DIB section whose pixel memory is directly accessible.
	 *
	 * @details Selecting the returned bitmap into a DC makes drawing land
	 * directly in *ppvBits — the readback copy a GetDIBits path performs
	 * never happens.
	 *
	 * @param hdc Handle to a device context (color format reference).
	 * @param pbmi Bitmap dimensions and format.
	 * @param usage Color table type (DIB_RGB_COLORS).
	 * @param ppvBits Receives the pointer to the bitmap bits.
	 * @param hSection File-mapping handle for a shared-memory DIB (nullptr for plain memory).
	 * @param offset Offset into hSection (ignored when hSection is nullptr).
	 * @return Handle to the DIB section bitmap, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see CreateDIBSection
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection
	 */
	[[nodiscard]] static PVOID CreateDIBSection(PVOID hdc, const BITMAPINFO *pbmi, UINT32 usage, PPVOID ppvBits, PVOID hSection, UINT32 offset);

	/**
	 * @brief Selects a GDI object into the specified device context.
	 *
	 * @param hdc Handle to the device context.
	 * @param h Handle to the GDI object to select.
	 * @return Handle to the previously selected object, or NULL on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see SelectObject
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-selectobject
	 */
	static PVOID SelectObject(PVOID hdc, PVOID h);

	/**
	 * @brief Performs a bit-block transfer between device contexts.
	 *
	 * @param hdc Destination device context.
	 * @param x Destination upper-left corner X coordinate.
	 * @param y Destination upper-left corner Y coordinate.
	 * @param cx Width of the source and destination rectangles.
	 * @param cy Height of the source and destination rectangles.
	 * @param hdcSrc Source device context.
	 * @param x1 Source upper-left corner X coordinate.
	 * @param y1 Source upper-left corner Y coordinate.
	 * @param rop Raster operation code (e.g., SRCCOPY).
	 * @return true on success, false on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see BitBlt
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
	 */
	[[nodiscard]] static BOOL BitBlt(PVOID hdc, INT32 x, INT32 y, INT32 cx, INT32 cy, PVOID hdcSrc, INT32 x1, INT32 y1, UINT32 rop);

	/**
	 * @brief Retrieves the bits of a bitmap and copies them into a buffer.
	 *
	 * @param hdc Handle to the device context.
	 * @param hbm Handle to the bitmap.
	 * @param start First scan line to retrieve.
	 * @param cLines Number of scan lines to retrieve.
	 * @param lpvBits Output buffer for pixel data, or NULL to fill bmi only.
	 * @param lpbmi Pointer to BITMAPINFOHEADER specifying the desired format.
	 * @param usage Color table type (DIB_RGB_COLORS or DIB_PAL_COLORS).
	 * @return Number of scan lines copied, or 0 on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see GetDIBits
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdibits
	 */
	[[nodiscard]] static INT32 GetDIBits(PVOID hdc, PVOID hbm, UINT32 start, UINT32 cLines, PVOID lpvBits, PBITMAPINFOHEADER lpbmi, UINT32 usage);

	/**
	 * @brief Deletes a device context.
	 *
	 * @param hdc Handle to the device context to delete.
	 * @return true on success, false on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see DeleteDC
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-deletedc
	 */
	static BOOL DeleteDC(PVOID hdc);

	/**
	 * @brief Deletes a GDI object (bitmap, brush, font, pen, or region).
	 *
	 * @param ho Handle to the GDI object to delete.
	 * @return true on success, false on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 *
	 * @see DeleteObject
	 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-deleteobject
	 */
	static BOOL DeleteObject(PVOID ho);
};
