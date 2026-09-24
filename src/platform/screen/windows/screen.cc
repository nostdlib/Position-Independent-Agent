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

// Persistent per-display capture resources: GDI object creation dominates a
// per-call capture, so repeated captures reuse one memory DC, bitmap, and
// BGRA conversion buffer (rebuilt when the display mode changes)
struct WinCaptureState
{
	PVOID memDC;
	PVOID bitmap;
	PVOID oldBitmap;
	BITMAPINFOHEADER bmi;
	UINT8 *bgra;
	UINT32 bgraSize;
	INT32 width;
	INT32 height;
};

// Deselect, delete, and null the owned GDI objects (bgra is left to the caller)
static VOID ReleaseGdiObjects(WinCaptureState *state)
{
	if (state->memDC != nullptr && state->oldBitmap != nullptr)
	{
		Gdi32::SelectObject(state->memDC, state->oldBitmap);
		state->oldBitmap = nullptr;
	}
	if (state->bitmap != nullptr)
	{
		Gdi32::DeleteObject(state->bitmap);
		state->bitmap = nullptr;
	}
	if (state->memDC != nullptr)
	{
		Gdi32::DeleteDC(state->memDC);
		state->memDC = nullptr;
	}
}

// (Re)create the compatible memory DC + bitmap bound to the given dimensions
static BOOL RebuildGdiObjects(WinCaptureState *state, INT32 width, INT32 height)
{
	ReleaseGdiObjects(state);

	PVOID screenDC = User32::GetDC(nullptr);
	if (screenDC == nullptr)
		return false;

	state->memDC = Gdi32::CreateCompatibleDC(screenDC);
	if (state->memDC != nullptr)
	{
		state->bitmap = Gdi32::CreateCompatibleBitmap(screenDC, width, height);
		if (state->bitmap != nullptr)
			state->oldBitmap = Gdi32::SelectObject(state->memDC, state->bitmap);
	}
	User32::ReleaseDC(nullptr, screenDC);

	if (state->memDC == nullptr || state->bitmap == nullptr || state->oldBitmap == nullptr)
	{
		ReleaseGdiObjects(state);
		return false;
	}

	// 32bpp top-down BITMAPINFOHEADER for GetDIBits
	Memory::Zero(&state->bmi, sizeof(state->bmi));
	state->bmi.biSize = sizeof(BITMAPINFOHEADER);
	state->bmi.biWidth = width;
	state->bmi.biHeight = -height; // negative = top-down scanlines
	state->bmi.biPlanes = 1;
	state->bmi.biBitCount = 32;
	state->bmi.biCompression = BI_RGB;

	state->width = width;
	state->height = height;
	return true;
}

// Stateless path: creates and destroys every GDI object per call
static Result<VOID, Error> CaptureStateless(const ScreenDevice &device, Span<RGB> buffer)
{
	INT32 width = (INT32)device.Width;
	INT32 height = (INT32)device.Height;

	PVOID screenDC = User32::GetDC(nullptr);
	if (screenDC == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

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

	// GetDIBits requires the bitmap not be selected into a DC (documented
	// precondition — some drivers enforce it)
	Gdi32::SelectObject(memDC, oldBitmap);
	INT32 scanLines = Gdi32::GetDIBits(memDC, bitmap, 0, (UINT32)height,
		tempBuf, &bmi, DIB_RGB_COLORS);
	Gdi32::SelectObject(memDC, bitmap);

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

Result<PVOID, Error> Screen::CreateCaptureState(const ScreenDevice &device)
{
	WinCaptureState *state = new WinCaptureState();
	if (state == nullptr)
		return Result<PVOID, Error>::Err(Error(Error::Screen_AllocFailed));
	Memory::Zero(state, sizeof(WinCaptureState));

	if (!RebuildGdiObjects(state, (INT32)device.Width, (INT32)device.Height))
	{
		delete state;
		return Result<PVOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	state->bgraSize = device.Width * device.Height * 4;
	state->bgra = new UINT8[state->bgraSize];
	if (state->bgra == nullptr)
	{
		ReleaseGdiObjects(state);
		delete state;
		return Result<PVOID, Error>::Err(Error(Error::Screen_AllocFailed));
	}

	return Result<PVOID, Error>::Ok((PVOID)state);
}

VOID Screen::DestroyCaptureState(PVOID captureState)
{
	if (captureState == nullptr)
		return;

	WinCaptureState *state = (WinCaptureState *)captureState;
	ReleaseGdiObjects(state);
	delete[] state->bgra;
	delete state;
}

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer, PVOID captureState)
{
	// One-shot callers (tests, single captures) take the create-per-call path
	if (captureState == nullptr)
		return CaptureStateless(device, buffer);

	WinCaptureState *state = (WinCaptureState *)captureState;
	INT32 width = (INT32)device.Width;
	INT32 height = (INT32)device.Height;
	UINT32 bgraNeeded = device.Width * device.Height * 4;

	// Display-mode change since the state was built: rebuild GDI objects and
	// grow the conversion buffer if the new mode is larger
	if (width != state->width || height != state->height || state->bgraSize < bgraNeeded)
	{
		if (!RebuildGdiObjects(state, width, height))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

		if (state->bgraSize < bgraNeeded)
		{
			UINT8 *grown = new UINT8[bgraNeeded];
			if (grown == nullptr)
				return Result<VOID, Error>::Err(Error(Error::Screen_AllocFailed));
			delete[] state->bgra;
			state->bgra = grown;
			state->bgraSize = bgraNeeded;
		}
	}

	PVOID screenDC = User32::GetDC(nullptr);
	if (screenDC == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	if (!Gdi32::BitBlt(state->memDC, 0, 0, width, height,
		screenDC, device.Left, device.Top, SRCCOPY))
	{
		User32::ReleaseDC(nullptr, screenDC);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}
	User32::ReleaseDC(nullptr, screenDC);

	// GetDIBits requires the bitmap not be selected into a DC (documented
	// precondition — some drivers enforce it)
	Gdi32::SelectObject(state->memDC, state->oldBitmap);
	INT32 scanLines = Gdi32::GetDIBits(state->memDC, state->bitmap, 0, (UINT32)height,
		state->bgra, &state->bmi, DIB_RGB_COLORS);
	Gdi32::SelectObject(state->memDC, state->bitmap);
	if (scanLines == 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Convert BGRA → RGB
	UINT32 pixelCount = device.Width * device.Height;
	PRGB rgbBuf = buffer.Data();
	for (UINT32 i = 0; i < pixelCount; i++)
	{
		UINT32 offset = i * 4;
		rgbBuf[i].Red = state->bgra[offset + 2];
		rgbBuf[i].Green = state->bgra[offset + 1];
		rgbBuf[i].Blue = state->bgra[offset];
	}

	return Result<VOID, Error>::Ok();
}
