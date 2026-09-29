#include "platform/kernel/windows/user32.h"
#include "platform/platform.h"
#include "platform/kernel/windows/peb.h"

#define ResolveUser32ExportAddress(functionName) ResolveExportAddress((const WCHAR *)L"user32.dll", Djb2::HashCompileTime(functionName))

BOOL User32::EnumDisplayDevicesW(const WCHAR *lpDevice, UINT32 iDevNum, PDISPLAY_DEVICEW lpDisplayDevice, UINT32 dwFlags)
{
	auto fn = (BOOL(STDCALL *)(const WCHAR *, UINT32, PDISPLAY_DEVICEW, UINT32))ResolveUser32ExportAddress("EnumDisplayDevicesW");
	if (fn == nullptr)
		return false;
	return fn(lpDevice, iDevNum, lpDisplayDevice, dwFlags);
}

BOOL User32::EnumDisplaySettingsW(const WCHAR *lpszDeviceName, UINT32 iModeNum, PDEVMODEW lpDevMode)
{
	auto fn = (BOOL(STDCALL *)(const WCHAR *, UINT32, PDEVMODEW))ResolveUser32ExportAddress("EnumDisplaySettingsW");
	if (fn == nullptr)
		return false;
	return fn(lpszDeviceName, iModeNum, lpDevMode);
}

PVOID User32::GetDC(PVOID hWnd)
{
	auto fn = (PVOID(STDCALL *)(PVOID))ResolveUser32ExportAddress("GetDC");
	if (fn == nullptr)
		return nullptr;
	return fn(hWnd);
}

INT32 User32::ReleaseDC(PVOID hWnd, PVOID hDC)
{
	auto fn = (INT32(STDCALL *)(PVOID, PVOID))ResolveUser32ExportAddress("ReleaseDC");
	if (fn == nullptr)
		return 0;
	return fn(hWnd, hDC);
}
