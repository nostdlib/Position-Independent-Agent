#include "platform/platform.h"
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"


NO_RETURN VOID ExitProcess(USIZE code)
{
	System::Call(SYS_EXIT, code);
	__builtin_unreachable();
}
