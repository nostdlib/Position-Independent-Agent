#include "platform/kernel/windows/kernel32.h"
#include "platform/platform.h"
#include "platform/kernel/windows/peb.h"

#define ResolveKernel32ExportAddress(functionName) ResolveExportAddressFromPebModule(Djb2::HashCompileTime(L"kernel32.dll"), Djb2::HashCompileTime(functionName))

Result<VOID, Error> Kernel32::CreateProcessW(PWCHAR lpApplicationName, PWCHAR lpCommandLine, PVOID lpProcessAttributes, PVOID lpThreadAttributes, BOOL bInheritHandles, UINT32 dwCreationFlags, PVOID lpEnvironment, PWCHAR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
	BOOL result = ((BOOL(STDCALL *)(PWCHAR lpApplicationName, PWCHAR lpCommandLine, PVOID lpProcessAttributes, PVOID lpThreadAttributes, BOOL bInheritHandles, UINT32 dwCreationFlags, PVOID lpEnvironment, PWCHAR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation))ResolveKernel32ExportAddress("CreateProcessW"))(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	if (!result)
	{
		return Result<VOID, Error>::Err(Error(Error::Kernel32_CreateProcessFailed));
	}
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Kernel32::SetHandleInformation(PVOID hObject, UINT32 dwMask, UINT32 dwFlags)
{
	BOOL result = ((BOOL(STDCALL *)(PVOID hObject, UINT32 dwMask, UINT32 dwFlags))ResolveKernel32ExportAddress("SetHandleInformation"))(hObject, dwMask, dwFlags);
	if (!result)
	{
		return Result<VOID, Error>::Err(Error(Error::Kernel32_SetHandleInfoFailed));
	}
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Kernel32::CreatePipe(PPVOID hReadPipe, PPVOID hWritePipe, PVOID lpPipeAttributes, UINT32 nSize)
{
	BOOL result = ((BOOL(STDCALL *)(PPVOID hReadPipe, PPVOID hWritePipe, PVOID lpPipeAttributes, UINT32 nSize))ResolveKernel32ExportAddress("CreatePipe"))(hReadPipe, hWritePipe, lpPipeAttributes, nSize);
	if (!result)
	{
		return Result<VOID, Error>::Err(Error(Error::Kernel32_CreatePipeFailed));
	}
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Kernel32::PeekNamedPipe(SSIZE hNamedPipe, PVOID lpBuffer, UINT32 nBufferSize, PUINT32 lpBytesRead, PUINT32 lpTotalBytesAvail, PUINT32 lpBytesLeftThisMessage)
{
	BOOL result = ((BOOL(STDCALL *)(SSIZE hNamedPipe, PVOID lpBuffer, UINT32 nBufferSize, PUINT32 lpBytesRead, PUINT32 lpTotalBytesAvail, PUINT32 lpBytesLeftThisMessage))ResolveKernel32ExportAddress("PeekNamedPipe"))(hNamedPipe, lpBuffer, nBufferSize, lpBytesRead, lpTotalBytesAvail, lpBytesLeftThisMessage);
	if (!result)
	{
		return Result<VOID, Error>::Err(Error(Error::Kernel32_PeekNamedPipeFailed));
	}
	return Result<VOID, Error>::Ok();
}