/**
 * @file screen.cc
 * @brief Windows Screen Implementation
 *
 * @details Implements screen device enumeration via User32
 * EnumDisplayDevicesW/EnumDisplaySettingsW and screen capture via
 * GDI CreateCompatibleDC/BitBlt/GetDIBits. User32 and Gdi32 wrappers
 * auto-load their DLLs via ResolveExportAddress when not already loaded.
 *
 * @see EnumDisplayDevicesW
 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw
 * @see EnumDisplaySettingsW
 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw
 * @see BitBlt
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
 * @see GetDIBits
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdibits
 */

#include "platform/screen/screen.h"
#include "platform/kernel/windows/user32.h"
#include "platform/kernel/windows/gdi32.h"

// =============================================================================
// Screen::GetDevices
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	// Enumerate active displays (stack buffer for up to 16)
	constexpr UINT32 maxDevices = 16;
	ScreenDevice tempDevices[maxDevices];
	UINT32 deviceCount = 0;

	for (UINT32 i = 0; i < maxDevices; i++)
	{
		DISPLAY_DEVICEW dd;
		Memory::Zero(&dd, sizeof(dd));
		dd.cb = sizeof(DISPLAY_DEVICEW);

		if (!User32::EnumDisplayDevicesW(nullptr, i, &dd, 0))
			break;

		if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
			continue;

		DEVMODEW dm;
		Memory::Zero(&dm, sizeof(dm));
		dm.dmSize = sizeof(DEVMODEW);

		if (!User32::EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
			continue;

		tempDevices[deviceCount].Left = dm.dmPositionX;
		tempDevices[deviceCount].Top = dm.dmPositionY;
		tempDevices[deviceCount].Width = dm.dmPelsWidth;
		tempDevices[deviceCount].Height = dm.dmPelsHeight;
		tempDevices[deviceCount].Primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
		deviceCount++;
	}

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
// Screen::Capture
// =============================================================================

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer)
{
	INT32 width = (INT32)device.Width;
	INT32 height = (INT32)device.Height;

	// Get the screen device context (NULL = entire virtual screen)
	PVOID screenDC = User32::GetDC(nullptr);
	if (screenDC == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Create memory DC and compatible bitmap
	PVOID memDC = Gdi32::CreateCompatibleDC(screenDC);
	if (memDC == nullptr)
	{
		User32::ReleaseDC(nullptr, screenDC);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	PVOID bitmap = Gdi32::CreateCompatibleBitmap(screenDC, width, height);
	if (bitmap == nullptr)
	{
		Gdi32::DeleteDC(memDC);
		User32::ReleaseDC(nullptr, screenDC);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	PVOID oldBitmap = Gdi32::SelectObject(memDC, bitmap);

	// Copy from screen to memory DC
	if (!Gdi32::BitBlt(memDC, 0, 0, width, height,
		screenDC, device.Left, device.Top, SRCCOPY))
	{
		Gdi32::SelectObject(memDC, oldBitmap);
		Gdi32::DeleteObject(bitmap);
		Gdi32::DeleteDC(memDC);
		User32::ReleaseDC(nullptr, screenDC);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Allocate temporary 32bpp buffer for GetDIBits (BGRA format)
	UINT32 pixelCount = device.Width * device.Height;
	UINT8 *tempBuf = new UINT8[pixelCount * 4];
	if (tempBuf == nullptr)
	{
		Gdi32::SelectObject(memDC, oldBitmap);
		Gdi32::DeleteObject(bitmap);
		Gdi32::DeleteDC(memDC);
		User32::ReleaseDC(nullptr, screenDC);
		return Result<VOID, Error>::Err(Error(Error::Screen_AllocFailed));
	}

	// Set up BITMAPINFOHEADER for 32bpp top-down
	BITMAPINFOHEADER bmi;
	Memory::Zero(&bmi, sizeof(bmi));
	bmi.biSize = sizeof(BITMAPINFOHEADER);
	bmi.biWidth = width;
	bmi.biHeight = -height; // negative = top-down scanlines
	bmi.biPlanes = 1;
	bmi.biBitCount = 32;
	bmi.biCompression = BI_RGB;

	INT32 scanLines = Gdi32::GetDIBits(memDC, bitmap, 0, (UINT32)height,
		tempBuf, &bmi, DIB_RGB_COLORS);

	// Cleanup GDI resources
	Gdi32::SelectObject(memDC, oldBitmap);
	Gdi32::DeleteObject(bitmap);
	Gdi32::DeleteDC(memDC);
	User32::ReleaseDC(nullptr, screenDC);

	if (scanLines == 0)
	{
		delete[] tempBuf;
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Convert BGRA → RGB
	PRGB rgbBuf = buffer.Data();
	for (UINT32 i = 0; i < pixelCount; i++)
	{
		UINT32 offset = i * 4;
		rgbBuf[i].Red = tempBuf[offset + 2];
		rgbBuf[i].Green = tempBuf[offset + 1];
		rgbBuf[i].Blue = tempBuf[offset];
	}

	delete[] tempBuf;
	return Result<VOID, Error>::Ok();
}
