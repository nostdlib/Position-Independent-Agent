/**
 * @file screen.cc
 * @brief Windows Screen Implementation
 *
 * @details Implements screen device enumeration via User32
 * EnumDisplayDevicesW/EnumDisplaySettingsW and screen capture via
 * GDI CreateCompatibleDC/BitBlt/GetDIBits. User32 and Gdi32 wrappers
 * auto-load their DLLs via ResolveExportAddress when not already loaded.
 * Each stateful capture logs a per-stage timing line so live runs show
 * where the frame time goes (blt / dibits / convert).
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
#include "platform/console/logger.h"
#include "platform/system/date_time.h"

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

/// @brief Convert a top-down 32bpp BGRA buffer to packed RGB in one pass
/// @details Four pixels per iteration pack into three dword stores instead
///          of twelve byte stores. An SSE2 shuffle network was measured
///          slower here (0.81 vs 0.67 ms per 1080p frame): the stage is
///          memory-bandwidth-bound, so fewer stores beat wider loads.
/// @param bgra GetDIBits output (4 bytes per pixel)
/// @param rgb Destination frame buffer (3 bytes per pixel)
/// @param pixelCount Total pixels to convert
static VOID ConvertBgraToRgb(const UINT8 *bgra, PRGB rgb, UINT32 pixelCount)
{
	UINT32 i = 0;
	for (; i + 4 <= pixelCount; i += 4, bgra += 16)
	{
		const UINT32 *src = (const UINT32 *)bgra;
		UINT32 *out = (UINT32 *)(rgb + i);
		UINT32 d0 = src[0], d1 = src[1], d2 = src[2], d3 = src[3];
		UINT32 r0 = (d0 >> 16) & 0xFF, g0 = (d0 >> 8) & 0xFF;
		UINT32 r1 = (d1 >> 16) & 0xFF, g1 = (d1 >> 8) & 0xFF, b1 = d1 & 0xFF;
		UINT32 r2 = (d2 >> 16) & 0xFF, g2 = (d2 >> 8) & 0xFF, b2 = d2 & 0xFF;
		UINT32 r3 = (d3 >> 16) & 0xFF, g3 = (d3 >> 8) & 0xFF, b3 = d3 & 0xFF;
		out[0] = r0 | (g0 << 8) | ((d0 & 0xFF) << 16) | (r1 << 24);
		out[1] = g1 | (b1 << 8) | (r2 << 16) | (g2 << 24);
		out[2] = b2 | (r3 << 8) | (g3 << 16) | (b3 << 24);
	}

	// Tail (last <4 pixels)
	for (; i < pixelCount; i++, bgra += 4)
	{
		rgb[i].Red = bgra[2];
		rgb[i].Green = bgra[1];
		rgb[i].Blue = bgra[0];
	}
}

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer, PVOID captureState)
{
	// One-shot callers (tests, single captures) run the same sequence through
	// a temporary state — one GDI pipeline to maintain
	if (captureState == nullptr)
	{
		auto state = CreateCaptureState(device);
		if (!state)
			return Result<VOID, Error>::Err(state.Error());
		auto result = Capture(device, buffer, state.Value());
		DestroyCaptureState(state.Value());
		return result;
	}

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

	// Persistent objects may go stale (lost DC, driver hiccup): rebuild once
	// and retry before reporting failure. blt covers GetDC..ReleaseDC around
	// the BitBlt, dibits the deselect/GetDIBits/reselect pair
	[[maybe_unused]] UINT64 bltNs = 0;
	[[maybe_unused]] UINT64 dibitsNs = 0;
	INT32 scanLines = 0;
	for (UINT32 attempt = 0; ; attempt++)
	{
		UINT64 stage = DateTime::GetMonotonicNanoseconds();
		PVOID screenDC = User32::GetDC(nullptr);
		if (screenDC == nullptr)
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

		BOOL blit = Gdi32::BitBlt(state->memDC, 0, 0, width, height,
			screenDC, device.Left, device.Top, SRCCOPY);
		User32::ReleaseDC(nullptr, screenDC);
		bltNs = DateTime::GetMonotonicNanoseconds() - stage;

		if (blit)
		{
			// GetDIBits requires the bitmap not be selected into a DC
			// (documented precondition — some drivers enforce it)
			stage = DateTime::GetMonotonicNanoseconds();
			Gdi32::SelectObject(state->memDC, state->oldBitmap);
			scanLines = Gdi32::GetDIBits(state->memDC, state->bitmap, 0, (UINT32)height,
				state->bgra, &state->bmi, DIB_RGB_COLORS);
			Gdi32::SelectObject(state->memDC, state->bitmap);
			dibitsNs = DateTime::GetMonotonicNanoseconds() - stage;
		}

		if (scanLines != 0)
			break;

		if (attempt >= 1 || !RebuildGdiObjects(state, width, height))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// BGRA -> RGB straight out of the GetDIBits buffer into the frame buffer
	UINT32 pixelCount = device.Width * device.Height;
	UINT64 stage = DateTime::GetMonotonicNanoseconds();
	ConvertBgraToRgb(state->bgra, buffer.Data(), pixelCount);
	[[maybe_unused]] UINT64 convertNs = DateTime::GetMonotonicNanoseconds() - stage;

	LOG_INFO("[capture] blt %u ms, dibits %u ms, convert %u ms",
	         (UINT32)(bltNs / 1000000), (UINT32)(dibitsNs / 1000000),
	         (UINT32)(convertNs / 1000000));

	return Result<VOID, Error>::Ok();
}
