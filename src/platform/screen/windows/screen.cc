/**
 * @file screen.cc
 * @brief Windows Screen Implementation
 *
 * @details Implements screen device enumeration via User32
 * EnumDisplayDevicesW/EnumDisplaySettingsW and screen capture via
 * GDI CreateCompatibleDC/BitBlt into a DIB section (with automatic fallback
 * to the CreateCompatibleBitmap + GetDIBits path when a driver rejects the
 * DIB destination). User32 and Gdi32 wrappers auto-load their DLLs via
 * ResolveExportAddress when not already loaded. Each stateful capture logs a
 * per-stage timing line so live runs show where the frame time goes.
 *
 * @see EnumDisplayDevicesW
 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw
 * @see EnumDisplaySettingsW
 *      https://learn.microsoft.com/en-us/windows/winuser/nf-winuser-enumdisplaysettingsw
 * @see BitBlt
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
 * @see CreateDIBSection
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection
 */

#include "platform/screen/screen.h"
#include "platform/kernel/windows/user32.h"
#include "platform/kernel/windows/gdi32.h"
#include "platform/kernel/windows/kernel32.h"
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

		// Degenerate mode (phantom/headless adapter): GDI object creation
		// fails on zero or absurd dimensions, so the display is unusable
		if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0 ||
			dm.dmPelsWidth > 32768 || dm.dmPelsHeight > 32768)
		{
			LOG_WARNING("skipping display %ws with degenerate mode %ux%u",
			            dd.DeviceName, dm.dmPelsWidth, dm.dmPelsHeight);
			continue;
		}

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

// Persistent per-display capture resources: repeated captures reuse one memory
// DC, a cached screen DC, and a capture bitmap. In DIB mode the bitmap is a
// DIB section whose pixel memory IS the BGRA buffer (BitBlt lands pixels
// directly in it — no GetDIBits round trip); in DDB mode it is a compatible
// bitmap read back with GetDIBits into a separate heap buffer. Everything is
// rebuilt when the display mode changes or a blt goes stale
struct WinCaptureState
{
	PVOID memDC;
	PVOID screenDC;          ///< Cached source DC; re-acquired on staleness
	PVOID bitmap;            ///< DIB section (DIB mode) or compatible bitmap (DDB mode)
	PVOID oldBitmap;
	BITMAPINFOHEADER bmi;
	UINT32 bmiMasks[3];      ///< 16bpp channel masks; must directly follow bmi in memory
	UINT8 *bgra;             ///< DIB-section bits or heap readback buffer (see bgraIsDibSection)
	UINT32 bgraSize;
	INT32 width;
	INT32 height;
	BOOL useDib;             ///< DIB-section blt target; flipped off on driver rejection
	BOOL bgraIsDibSection;   ///< bgra freed via DeleteObject (true) or delete[] (false)
	BOOL probeRan;           ///< Probe executed; the classification fields below are valid
	BOOL dib16Allowed;       ///< Probe: 16bpp blt meaningfully cheaper than 32bpp
	BOOL gateAllowed;        ///< Probe: scaled change gate viable on this driver
	INT32 gateMode;          ///< Gate stretch mode (COLORONCOLOR or HALFTONE)
	BOOL gateHalftoneOk;     ///< SetStretchBltMode(HALFTONE) accepted by the driver
	WCHAR deviceName[32];    ///< This display's \\.\DISPLAYN name for a private source DC
	BOOL deviceNameValid;    ///< deviceName was matched to this display's geometry
	BOOL screenDcPrivate;    ///< screenDC from CreateDCW (DeleteDC) vs GetDC (ReleaseDC)
	INT32 srcX;              ///< Blt source origin: (0,0) for a per-monitor DC, else virtual position
	INT32 srcY;
	INT32 virtualX;          ///< Display position in the virtual screen (shared-DC blt origin)
	INT32 virtualY;
	UINT16 bpp;              ///< Current capture depth (32 or 16)
	PVOID gateMemDC;         ///< Change-gate DC (a bitmap selects into only one DC)
	PVOID gateBitmap;        ///< Quarter-resolution 32bpp gate DIB section
	PVOID gateOldBitmap;
	UINT8 *gateBits;         ///< gateBitmap's section bits
	UINT8 *gatePrev;         ///< Heap copy of the previous gate frame
	UINT32 gateWidth;        ///< Gate dimensions in pixels (display / 4)
	UINT32 gateHeight;
	BOOL gateValid;          ///< gatePrev holds a frame comparable to the next gate capture
	UINT32 gateSkipStreak;   ///< Consecutive gate-only frames; bounds sub-block staleness
};

// One full capture between gate-only streaks: changes too small to survive
// quarter-scale downsampling surface within this many frames
static constexpr UINT32 GateResyncInterval = 15;

// Free the change-gate objects; the next gated capture rebuilds them lazily
static VOID DestroyGateObjects(WinCaptureState *state)
{
	if (state->gateMemDC != nullptr && state->gateOldBitmap != nullptr)
		Gdi32::SelectObject(state->gateMemDC, state->gateOldBitmap);
	if (state->gateBitmap != nullptr)
		Gdi32::DeleteObject(state->gateBitmap);
	if (state->gateMemDC != nullptr)
		Gdi32::DeleteDC(state->gateMemDC);
	state->gateMemDC = nullptr;
	state->gateBitmap = nullptr;
	state->gateOldBitmap = nullptr;
	state->gateBits = nullptr;
	if (state->gatePrev != nullptr)
	{
		delete[] state->gatePrev;
		state->gatePrev = nullptr;
	}
	state->gateValid = false;
	state->gateSkipStreak = 0;
	state->gateWidth = 0;
	state->gateHeight = 0;
}

// Gate frames differ at any dword — StretchBlt is deterministic for identical
// input, so exact equality is a sound "screen unchanged" predicate
static BOOL GateFramesDiffer(const UINT8 *a, const UINT8 *b, USIZE dwordCount)
{
	const UINT32 *x = (const UINT32 *)a;
	const UINT32 *y = (const UINT32 *)b;
	for (USIZE i = 0; i < dwordCount; i++)
	{
		if (x[i] != y[i])
			return true;
	}
	return false;
}

// Deselect, delete, and null the owned GDI objects and pixel buffer. A DIB
// section's memory dies with its handle; the DDB-mode readback buffer is heap
static VOID ReleaseGdiObjects(WinCaptureState *state)
{
	DestroyGateObjects(state);

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
	if (state->bgra != nullptr && !state->bgraIsDibSection)
		delete[] state->bgra;
	state->bgra = nullptr;
	state->bgraSize = 0;
	state->bgraIsDibSection = false;
	if (state->memDC != nullptr)
	{
		Gdi32::DeleteDC(state->memDC);
		state->memDC = nullptr;
	}
}

// Drop the cached source DC; the next acquire creates a fresh one
static VOID DropScreenDC(WinCaptureState *state)
{
	if (state->screenDC != nullptr)
	{
		if (state->screenDcPrivate)
			Gdi32::DeleteDC(state->screenDC);
		else
			User32::ReleaseDC(nullptr, state->screenDC);
		state->screenDC = nullptr;
	}
}

// Acquire the state's source DC. Preferred: a per-monitor private DC
// (CreateDCW on the matched device name — its surface is exactly that
// monitor, blt origin (0,0), contract-legal to cache, and reachable on
// drivers where the shared GetDC(nullptr) DC cannot source a non-primary
// display or accept a DIB target). Fallback: the shared virtual-screen DC
static PVOID AcquireScreenDC(WinCaptureState *state)
{
	if (state->screenDC != nullptr)
		return state->screenDC;

	if (state->deviceNameValid)
	{
		auto displayDriver = L"DISPLAY";
		state->screenDC = Gdi32::CreateDCW((PCWCHAR)displayDriver, state->deviceName, nullptr, nullptr);
		if (state->screenDC != nullptr)
		{
			state->screenDcPrivate = true;
			state->srcX = 0;
			state->srcY = 0;
			return state->screenDC;
		}
		LOG_WARNING("per-monitor dc creation failed, using the shared screen dc");
	}

	state->screenDC = User32::GetDC(nullptr);
	state->screenDcPrivate = false;
	state->srcX = state->virtualX;
	state->srcY = state->virtualY;
	return state->screenDC;
}

// Build the gate objects for the current display dimensions (idempotent)
static BOOL EnsureGateObjects(WinCaptureState *state)
{
	UINT32 wantW = (UINT32)(state->width / 4);
	UINT32 wantH = (UINT32)(state->height / 4);
	if (wantW == 0)
		wantW = 1;
	if (wantH == 0)
		wantH = 1;
	if (state->gateBitmap != nullptr && state->gatePrev != nullptr &&
		state->gateWidth == wantW && state->gateHeight == wantH)
		return true;

	DestroyGateObjects(state);

	PVOID screenDC = AcquireScreenDC(state);
	if (screenDC == nullptr)
	{
		DropScreenDC(state);
		return false;
	}

	state->gateMemDC = Gdi32::CreateCompatibleDC(screenDC);
	if (state->gateMemDC == nullptr)
	{
		DestroyGateObjects(state);
		return false;
	}

	BITMAPINFOHEADER bmi;
	Memory::Zero(&bmi, sizeof(bmi));
	bmi.biSize = sizeof(BITMAPINFOHEADER);
	bmi.biWidth = (INT32)wantW;
	bmi.biHeight = -(INT32)wantH; // top-down
	bmi.biPlanes = 1;
	bmi.biBitCount = 32;
	bmi.biCompression = BI_RGB;

	PVOID bits = nullptr;
	state->gateBitmap = Gdi32::CreateDIBSection(screenDC, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (state->gateBitmap == nullptr || bits == nullptr)
	{
		DestroyGateObjects(state);
		return false;
	}
	state->gateBits = (UINT8 *)bits;
	state->gateOldBitmap = Gdi32::SelectObject(state->gateMemDC, state->gateBitmap);
	if (state->gateOldBitmap == nullptr)
	{
		DestroyGateObjects(state);
		return false;
	}

	state->gatePrev = new UINT8[(USIZE)wantW * wantH * 4];
	if (state->gatePrev == nullptr)
	{
		DestroyGateObjects(state);
		return false;
	}

	state->gateWidth = wantW;
	state->gateHeight = wantH;
	return true;
}

// (Re)create the memory DC + capture bitmap for the current mode. DIB mode:
// the section's bits pointer becomes state->bgra, so blts into the DC write
// the capture straight into our buffer. DDB mode: plain compatible bitmap,
// read back with GetDIBits in Capture
static BOOL RebuildGdiObjects(WinCaptureState *state, INT32 width, INT32 height, UINT16 bpp)
{
	ReleaseGdiObjects(state);

	PVOID screenDC = AcquireScreenDC(state);
	if (screenDC == nullptr)
	{
		LOG_ERROR("gdi rebuild %dx%d: no source dc", width, height);
		return false;
	}

	state->memDC = Gdi32::CreateCompatibleDC(screenDC);
	if (state->memDC == nullptr)
		LOG_ERROR("gdi rebuild %dx%d: CreateCompatibleDC failed", width, height);
	if (state->memDC != nullptr)
	{
		// Top-down format shared by both modes (DIB target / GetDIBits). At
		// 16bpp the layout is pinned with explicit 5-6-5 masks (BI_RGB 16bpp
		// is documented ambiguously across eras); the masks must sit directly
		// behind the header in memory
		Memory::Zero(&state->bmi, sizeof(state->bmi));
		state->bmi.biSize = sizeof(BITMAPINFOHEADER);
		state->bmi.biWidth = width;
		state->bmi.biHeight = -height; // negative = top-down scanlines
		state->bmi.biPlanes = 1;
		state->bmi.biBitCount = bpp;
		if (bpp == 16)
		{
			state->bmi.biCompression = BI_BITFIELDS;
			state->bmiMasks[0] = 0xF800; // red
			state->bmiMasks[1] = 0x07E0; // green
			state->bmiMasks[2] = 0x001F; // blue
		}
		else
		{
			state->bmi.biCompression = BI_RGB;
			state->bmiMasks[0] = state->bmiMasks[1] = state->bmiMasks[2] = 0;
		}

		if (state->useDib)
		{
			// No color table at 32bpp BI_RGB, so the header alone is the BITMAPINFO
			PVOID bits = nullptr;
			BITMAPINFO *info = (BITMAPINFO *)&state->bmi;
			state->bitmap = Gdi32::CreateDIBSection(screenDC, info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (state->bitmap == nullptr || bits == nullptr)
				LOG_ERROR("gdi rebuild %dx%d: CreateDIBSection failed", width, height);
			else
			{
				state->bgra = (UINT8 *)bits;
				// DIB rows are DWORD-aligned: 16bpp odd widths carry padding
				state->bgraSize = (UINT32)((((USIZE)width * bpp + 31) / 32) * 4) * (UINT32)height;
				state->bgraIsDibSection = true;
				state->oldBitmap = Gdi32::SelectObject(state->memDC, state->bitmap);
				if (state->oldBitmap == nullptr)
					LOG_ERROR("gdi rebuild %dx%d: SelectObject (dib) failed", width, height);
			}
		}
		else
		{
			state->bitmap = Gdi32::CreateCompatibleBitmap(screenDC, width, height);
			if (state->bitmap == nullptr)
				LOG_ERROR("gdi rebuild %dx%d: CreateCompatibleBitmap failed", width, height);
			else
			{
				state->oldBitmap = Gdi32::SelectObject(state->memDC, state->bitmap);
				if (state->oldBitmap == nullptr)
					LOG_ERROR("gdi rebuild %dx%d: SelectObject failed", width, height);
			}
		}
	}

	BOOL ok = state->memDC != nullptr && state->bitmap != nullptr && state->oldBitmap != nullptr &&
	          (!state->useDib || state->bgra != nullptr);
	if (!ok)
	{
		ReleaseGdiObjects(state);
		return false;
	}

	state->width = width;
	state->height = height;
	state->bpp = bpp;
	return true;
}

// Rebuild with automatic degradation: some drivers reject the DIB-section
// blt target (observed for non-primary monitors) — capture must still work,
// so fall back to the proven DDB + GetDIBits path before reporting failure
static BOOL RebuildCaptureObjects(WinCaptureState *state, INT32 width, INT32 height, UINT16 bpp)
{
	if (RebuildGdiObjects(state, width, height, bpp))
		return true;
	if (state->useDib)
	{
		state->useDib = false;
		LOG_WARNING("dib-section capture rejected, falling back to getdibits");
		return RebuildGdiObjects(state, width, height, bpp);
	}
	return false;
}

// Match this display's \\.\DISPLAYN device name by re-enumerating and
// comparing geometry — the name enables a per-monitor private source DC.
// ScreenDevice carries no name (it is wire format), so the match is positional
static BOOL MatchDeviceName(const ScreenDevice &device, WCHAR *deviceName)
{
	constexpr UINT32 maxDevices = 16;
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

		if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0)
			continue;

		if (dm.dmPositionX == device.Left && dm.dmPositionY == device.Top &&
			dm.dmPelsWidth == device.Width && dm.dmPelsHeight == device.Height)
		{
			Memory::Copy(deviceName, dd.DeviceName, sizeof(dd.DeviceName));
			return true;
		}
	}
	return false;
}

// Scratch DC + top-down DIB section for probe measurements; destroyed locally
struct ScratchDib
{
	PVOID dc;
	PVOID bitmap;
	PVOID oldBitmap;
	UINT8 *bits;
};

static VOID DestroyScratchDib(ScratchDib *dib)
{
	if (dib->dc != nullptr && dib->oldBitmap != nullptr)
		Gdi32::SelectObject(dib->dc, dib->oldBitmap);
	if (dib->bitmap != nullptr)
		Gdi32::DeleteObject(dib->bitmap);
	if (dib->dc != nullptr)
		Gdi32::DeleteDC(dib->dc);
	Memory::Zero(dib, sizeof(ScratchDib));
}

static BOOL CreateScratchDib(ScratchDib *out, PVOID screenDC, INT32 width, INT32 height, UINT16 bpp)
{
	Memory::Zero(out, sizeof(ScratchDib));
	out->dc = Gdi32::CreateCompatibleDC(screenDC);
	if (out->dc == nullptr)
		return false;

	// Header + channel masks in one block: 16bpp pins 5-6-5 explicitly (the
	// masks must directly follow the header for BI_BITFIELDS); 32bpp BI_RGB
	// needs no masks
	struct
	{
		BITMAPINFOHEADER header;
		UINT32 bmiMasks[3];
	} info;
	Memory::Zero(&info, sizeof(info));
	info.header.biSize = sizeof(BITMAPINFOHEADER);
	info.header.biWidth = width;
	info.header.biHeight = -height; // top-down
	info.header.biPlanes = 1;
	info.header.biBitCount = bpp;
	if (bpp == 16)
	{
		info.header.biCompression = BI_BITFIELDS;
		info.bmiMasks[0] = 0xF800; // red
		info.bmiMasks[1] = 0x07E0; // green
		info.bmiMasks[2] = 0x001F; // blue
	}
	else
	{
		info.header.biCompression = BI_RGB;
	}

	PVOID bits = nullptr;
	out->bitmap = Gdi32::CreateDIBSection(screenDC, (BITMAPINFO *)&info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (out->bitmap == nullptr || bits == nullptr)
	{
		DestroyScratchDib(out);
		return false;
	}
	out->bits = (UINT8 *)bits;
	out->oldBitmap = Gdi32::SelectObject(out->dc, out->bitmap);
	if (out->oldBitmap == nullptr)
	{
		DestroyScratchDib(out);
		return false;
	}
	return true;
}

/// @brief Time a BitBlt best-of-3, in nanoseconds; 0 when any call fails
static UINT64 TimeBltBest3(PVOID dstDC, INT32 width, INT32 height, PVOID srcDC, INT32 srcX, INT32 srcY)
{
	UINT64 best = ~(UINT64)0;
	for (INT32 i = 0; i < 3; i++)
	{
		UINT64 start = DateTime::GetMonotonicNanoseconds();
		BOOL ok = Gdi32::BitBlt(dstDC, 0, 0, width, height, srcDC, srcX, srcY, SRCCOPY);
		UINT64 elapsed = DateTime::GetMonotonicNanoseconds() - start;
		if (!ok)
			return 0;
		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

/// @brief Time a full-frame downscaling StretchBlt best-of-3, in nanoseconds; 0 on failure
static UINT64 TimeStretchBest3(PVOID dstDC, INT32 dstW, INT32 dstH, PVOID srcDC, INT32 srcW, INT32 srcH, INT32 srcX, INT32 srcY)
{
	UINT64 best = ~(UINT64)0;
	for (INT32 i = 0; i < 3; i++)
	{
		UINT64 start = DateTime::GetMonotonicNanoseconds();
		BOOL ok = Gdi32::StretchBlt(dstDC, 0, 0, dstW, dstH, srcDC, srcX, srcY, srcW, srcH, SRCCOPY);
		UINT64 elapsed = DateTime::GetMonotonicNanoseconds() - start;
		if (!ok)
			return 0;
		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

// One-shot classification of this machine's blt cost curve, logged once per
// display: does capture cost track the copied size (bytes-bound — the 16bpp
// depth and the scaled change gate both pay) or is it a per-call floor
static VOID RunCaptureProbe(WinCaptureState *state)
{
	INT32 width = state->width;
	INT32 height = state->height;
	PVOID screenDC = AcquireScreenDC(state);
	if (screenDC == nullptr)
	{
		DropScreenDC(state);
		return;
	}

	// Warm the driver path so the first timed sample is not the cold one
	(VOID)Gdi32::BitBlt(state->memDC, 0, 0, width, height, screenDC, state->srcX, state->srcY, SRCCOPY);

	[[maybe_unused]] UINT64 fullNs = TimeBltBest3(state->memDC, width, height, screenDC, state->srcX, state->srcY);
	[[maybe_unused]] UINT64 crop50Ns = 0;
	[[maybe_unused]] UINT64 crop25Ns = 0;
	[[maybe_unused]] UINT64 stccNs = 0;
	[[maybe_unused]] UINT64 sthtNs = 0;
	[[maybe_unused]] UINT64 dib16Ns = 0;

	ScratchDib half;
	if (CreateScratchDib(&half, screenDC, width / 2, height / 2, 32))
	{
		crop50Ns = TimeBltBest3(half.dc, width / 2, height / 2, screenDC, state->srcX, state->srcY);
		DestroyScratchDib(&half);
	}

	ScratchDib quarter;
	if (CreateScratchDib(&quarter, screenDC, width / 4, height / 4, 32))
	{
		crop25Ns = TimeBltBest3(quarter.dc, width / 4, height / 4, screenDC, state->srcX, state->srcY);

		(VOID)Gdi32::SetStretchBltMode(quarter.dc, COLORONCOLOR);
		stccNs = TimeStretchBest3(quarter.dc, width / 4, height / 4, screenDC, width, height, state->srcX, state->srcY);

		// HALFTONE averages sub-blocks (better change sensitivity) instead of
		// decimating rows/columns; keep it only when the driver accepts it
		state->gateHalftoneOk = Gdi32::SetStretchBltMode(quarter.dc, HALFTONE) != 0;
		if (state->gateHalftoneOk)
			sthtNs = TimeStretchBest3(quarter.dc, width / 4, height / 4, screenDC, width, height, state->srcX, state->srcY);
		// Docs require SetBrushOrgEx after HALFTONE stretching; this DC never
		// selects a brush, so that call is deliberately skipped
		DestroyScratchDib(&quarter);
	}

	ScratchDib dib16;
	if (CreateScratchDib(&dib16, screenDC, width, height, 16))
	{
		dib16Ns = TimeBltBest3(dib16.dc, width, height, screenDC, state->srcX, state->srcY);
		DestroyScratchDib(&dib16);
	}

	[[maybe_unused]] UINT32 cpuCount = 0;
	auto cpus = Kernel32::GetProcessorCount();
	if (cpus)
		cpuCount = cpus.Value();
	[[maybe_unused]] INT32 remoteSession = User32::GetSystemMetrics(SM_REMOTESESSION);

	// Classification — integer ratios, spaced well beyond the ~1ms clock tick.
	// bytesBound: a quarter-linear crop is >= 2x cheaper than the full blt
	BOOL probeUsable = (fullNs != 0 && crop25Ns != 0);
	BOOL bytesBound = probeUsable && crop25Ns * 2 < fullNs;
	// 16bpp pays only when >= 25% cheaper (expect ~2x; some drivers convert internally)
	state->dib16Allowed = bytesBound && dib16Ns != 0 && dib16Ns * 4 < fullNs * 3;
	UINT64 stretchNs = (sthtNs != 0 && sthtNs < stccNs) ? sthtNs : stccNs;
	state->gateAllowed = probeUsable && stretchNs != 0 && stretchNs * 2 < fullNs && width >= 4 && height >= 4;
	state->gateMode = (state->gateHalftoneOk && sthtNs != 0 && stccNs != 0 && sthtNs * 2 <= stccNs * 3) ? HALFTONE : COLORONCOLOR;
	state->probeRan = true;

	LOG_INFO("[probe] full=%u crop50=%u crop25=%u stcc=%u stht=%u dib16=%u ms; cpu=%u rdp=%u; gate=%u mode=%u bpp16=%u (%ux%u)",
		(UINT32)(fullNs / 1000000), (UINT32)(crop50Ns / 1000000), (UINT32)(crop25Ns / 1000000),
		(UINT32)(stccNs / 1000000), (UINT32)(sthtNs / 1000000), (UINT32)(dib16Ns / 1000000),
		cpuCount, (UINT32)remoteSession,
		state->gateAllowed ? 1 : 0, (UINT32)state->gateMode, state->dib16Allowed ? 1 : 0,
		(UINT32)width, (UINT32)height);
}

Result<PVOID, Error> Screen::CreateCaptureState(const ScreenDevice &device)
{
	WinCaptureState *state = new WinCaptureState();
	if (state == nullptr)
		return Result<PVOID, Error>::Err(Error(Error::Screen_AllocFailed));
	Memory::Zero(state, sizeof(WinCaptureState));
	state->useDib = true;
	state->bpp = 32;
	state->virtualX = device.Left;
	state->virtualY = device.Top;
	state->srcX = device.Left;
	state->srcY = device.Top;
	state->deviceNameValid = MatchDeviceName(device, state->deviceName);

	if (!RebuildCaptureObjects(state, (INT32)device.Width, (INT32)device.Height, state->bpp))
	{
		DropScreenDC(state);
		delete state;
		return Result<PVOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	RunCaptureProbe(state);

	return Result<PVOID, Error>::Ok((PVOID)state);
}

VOID Screen::DestroyCaptureState(PVOID captureState)
{
	if (captureState == nullptr)
		return;

	WinCaptureState *state = (WinCaptureState *)captureState;
	ReleaseGdiObjects(state);
	DropScreenDC(state);
	delete state;
}

/// @brief Convert a top-down 32bpp BGRA buffer to packed RGB in one pass
/// @details Four pixels per iteration pack into three dword stores instead
///          of twelve byte stores. An SSE2 shuffle network was measured
///          slower here (0.81 vs 0.67 ms per 1080p frame): the stage is
///          memory-bandwidth-bound, so fewer stores beat wider loads.
/// @param bgra Capture pixel memory — DIB-section bits or GetDIBits buffer
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

/// @brief Convert a top-down 16bpp 5-6-5 (little-endian) buffer to packed RGB
/// @details Words are 5-6-5: blue = low 5 bits, green = middle 6, red = high
///          5 (pinned with explicit masks at DIB creation). Channels expand by
///          bit replication (no LUT, no division); four pixels per iteration
///          pack into three dword stores. Rows are read at the DIB stride —
///          DWORD-aligned, so odd widths carry 2 padding bytes per row.
/// @param bgr565 Capture pixel memory (2 bytes per pixel)
/// @param rgb Destination frame buffer (3 bytes per pixel)
/// @param width Frame width in pixels
/// @param height Frame height in pixels
/// @param strideBytes Row stride in bytes of bgr565
static VOID ConvertBgr565ToRgb(const UINT8 *bgr565, PRGB rgb, UINT32 width, UINT32 height, USIZE strideBytes)
{
	for (UINT32 row = 0; row < height; row++)
	{
		const UINT16 *src = (const UINT16 *)(bgr565 + (USIZE)row * strideBytes);
		PRGB dst = rgb + (USIZE)row * width;
		UINT32 i = 0;
		for (; i + 4 <= width; i += 4)
		{
			UINT32 *out = (UINT32 *)(dst + i);
			UINT32 p[4];
			for (INT32 k = 0; k < 4; k++)
			{
				UINT16 px = src[i + k];
				UINT32 r5 = (UINT32)((px >> 11) & 0x1F);
				UINT32 g6 = (UINT32)((px >> 5) & 0x3F);
				UINT32 b5 = (UINT32)(px & 0x1F);
				p[k] = ((r5 << 3) | (r5 >> 2)) | (((g6 << 2) | (g6 >> 4)) << 8) | (((b5 << 3) | (b5 >> 2)) << 16);
			}
			out[0] = p[0] | (p[1] << 24);
			out[1] = (p[1] >> 8) | (p[2] << 16);
			out[2] = (p[2] >> 16) | (p[3] << 8);
		}

		// Tail (last <4 pixels of the row)
		for (; i < width; i++)
		{
			UINT16 px = src[i];
			UINT32 r5 = (UINT32)((px >> 11) & 0x1F);
			UINT32 g6 = (UINT32)((px >> 5) & 0x3F);
			UINT32 b5 = (UINT32)(px & 0x1F);
			dst[i].Red = (UINT8)((r5 << 3) | (r5 >> 2));
			dst[i].Green = (UINT8)((g6 << 2) | (g6 >> 4));
			dst[i].Blue = (UINT8)((b5 << 3) | (b5 >> 2));
		}
	}
}

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer, PVOID captureState,
                                   const CaptureOptions *options, CaptureStatus *status)
{
	// One-shot callers (tests, single captures) run the same sequence through
	// a temporary state — one GDI pipeline to maintain
	if (captureState == nullptr)
	{
		auto state = CreateCaptureState(device);
		if (!state)
			return Result<VOID, Error>::Err(state.Error());
		auto result = Capture(device, buffer, state.Value(), options, status);
		DestroyCaptureState(state.Value());
		return result;
	}

	WinCaptureState *state = (WinCaptureState *)captureState;
	INT32 width = (INT32)device.Width;
	INT32 height = (INT32)device.Height;

	// Display-mode change since the state was built: rebuild everything
	if (width != state->width || height != state->height)
	{
		if (!RebuildCaptureObjects(state, width, height, state->bpp))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Depth switch (probe-gated): a 16bpp request only takes effect where the
	// probe measured it worthwhile; the diff base is invalid afterwards, so
	// the caller is told to reply a full frame
	UINT32 wantBpp = (options != nullptr && options->BitsPerPixel == 16 &&
	                  state->probeRan && state->dib16Allowed) ? 16 : 32;
	if (wantBpp != state->bpp)
	{
		if (!RebuildCaptureObjects(state, width, height, (UINT16)wantBpp))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		if (status != nullptr)
			status->DepthChanged = true;
	}

	// Change gate: a quarter-scale readback proves (or disproves) "the screen
	// did not change since the last gated frame" for a fraction of the full
	// blt's cost; a proven-unchanged frame skips the blt and the conversion
	// entirely, leaving the caller's frame buffer untouched
	BOOL gateSkipped = false;
	[[maybe_unused]] UINT64 gateNs = 0;
	BOOL gateEligible = (options != nullptr && options->AllowSkip && state->probeRan &&
	                     state->gateAllowed && (status == nullptr || !status->DepthChanged));
	if (gateEligible && EnsureGateObjects(state))
	{
		PVOID screenDC = AcquireScreenDC(state);
		if (screenDC == nullptr)
		{
			DropScreenDC(state);
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		UINT64 gateStart = DateTime::GetMonotonicNanoseconds();
		BOOL gateOk = Gdi32::SetStretchBltMode(state->gateMemDC, state->gateMode) != 0;
		if (gateOk)
			gateOk = Gdi32::StretchBlt(state->gateMemDC, 0, 0, (INT32)state->gateWidth, (INT32)state->gateHeight,
				screenDC, state->srcX, state->srcY, width, height, SRCCOPY);
		gateNs = DateTime::GetMonotonicNanoseconds() - gateStart;

		if (gateOk)
		{
			BOOL resyncDue = state->gateSkipStreak >= GateResyncInterval;
			if (state->gateValid && !resyncDue &&
			    !GateFramesDiffer(state->gateBits, state->gatePrev,
			                      (USIZE)state->gateWidth * state->gateHeight))
			{
				gateSkipped = true;
				state->gateSkipStreak++;
				if (status != nullptr)
					status->FrameUnchanged = true;
			}
			else
			{
				Memory::Copy(state->gatePrev, state->gateBits,
				             (USIZE)state->gateWidth * state->gateHeight * 4);
				state->gateValid = true;
				state->gateSkipStreak = 0;
			}
		}
		else
		{
			// The gate capture itself failed; its baseline is not trustworthy
			state->gateValid = false;
		}
	}
	else
	{
		// The gate did not run this frame: whatever reply the caller produces
		// moves the receiver independently of the gate's baseline, so the
		// baseline must not authorize a skip on the next frame
		state->gateValid = false;
	}

	if (status != nullptr)
		status->BitsPerPixel = state->bpp;

	if (gateSkipped)
	{
		LOG_INFO("[capture] blt %u ms, dibits %u ms, convert %u ms, bpp %u, gate %u ms, skip %u",
		         0, 0, 0, state->bpp, (UINT32)(gateNs / 1000000), 1);
		return Result<VOID, Error>::Ok();
	}

	// Persistent objects may go stale (lost DC, driver hiccup, desktop switch
	// invalidating the cached screen DC): drop the cached DC, rebuild once,
	// and retry before reporting failure. blt measures the BitBlt itself;
	// dibits stays 0 in DIB mode (captures land in bgra directly)
	[[maybe_unused]] UINT64 bltNs = 0;
	[[maybe_unused]] UINT64 dibitsNs = 0;
	for (UINT32 attempt = 0; ; attempt++)
	{
		PVOID screenDC = AcquireScreenDC(state);
		if (screenDC == nullptr)
		{
			DropScreenDC(state);
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		UINT64 stage = DateTime::GetMonotonicNanoseconds();
		BOOL blit = Gdi32::BitBlt(state->memDC, 0, 0, width, height,
			screenDC, state->srcX, state->srcY, SRCCOPY);
		bltNs = DateTime::GetMonotonicNanoseconds() - stage;

		if (!blit)
		{
			if (attempt >= 1)
				return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

			// Cached DC may be stale; a DIB target being rejected outright
			// degrades to the DDB + GetDIBits path for good
			DropScreenDC(state);
			if (state->useDib)
			{
				state->useDib = false;
				LOG_WARNING("dib-section blt rejected, falling back to getdibits");
			}
			if (!RebuildGdiObjects(state, width, height, state->bpp))
				return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
			continue;
		}

		if (state->useDib)
			break; // pixels are in state->bgra already

		// DDB destination: read the pixels back. GetDIBits requires the bitmap
		// not be selected into a DC (documented precondition — some drivers
		// enforce it)
		UINT32 bgraNeeded = (UINT32)((((USIZE)width * state->bpp + 31) / 32) * 4) * (UINT32)height;
		if (state->bgraSize < bgraNeeded)
		{
			UINT8 *grown = new UINT8[bgraNeeded];
			if (grown == nullptr)
				return Result<VOID, Error>::Err(Error(Error::Screen_AllocFailed));
			delete[] state->bgra; // heap-only here (DIB-mode memory died with the bitmap)
			state->bgra = grown;
			state->bgraSize = bgraNeeded;
		}

		stage = DateTime::GetMonotonicNanoseconds();
		Gdi32::SelectObject(state->memDC, state->oldBitmap);
		INT32 scanLines = Gdi32::GetDIBits(state->memDC, state->bitmap, 0, (UINT32)height,
			state->bgra, &state->bmi, DIB_RGB_COLORS);
		Gdi32::SelectObject(state->memDC, state->bitmap);
		dibitsNs = DateTime::GetMonotonicNanoseconds() - stage;
		if (scanLines != 0)
			break;

		if (attempt >= 1)
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

		DropScreenDC(state);
		if (!RebuildGdiObjects(state, width, height, state->bpp))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// BGRA/RGB565 -> RGB straight out of the capture memory into the frame buffer
	UINT32 pixelCount = device.Width * device.Height;
	USIZE captureStride = ((USIZE)width * state->bpp + 31) / 32 * 4;
	UINT64 stage = DateTime::GetMonotonicNanoseconds();
	if (state->bpp == 16)
		ConvertBgr565ToRgb(state->bgra, buffer.Data(), device.Width, device.Height, captureStride);
	else
		ConvertBgraToRgb(state->bgra, buffer.Data(), pixelCount);
	[[maybe_unused]] UINT64 convertNs = DateTime::GetMonotonicNanoseconds() - stage;

	LOG_INFO("[capture] blt %u ms, dibits %u ms, convert %u ms, bpp %u, gate %u ms, skip %u",
	         (UINT32)(bltNs / 1000000), (UINT32)(dibitsNs / 1000000),
	         (UINT32)(convertNs / 1000000), state->bpp, (UINT32)(gateNs / 1000000), 0);

	return Result<VOID, Error>::Ok();
}
