#include "platform/platform.h"
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"

NO_RETURN VOID ExitProcess(USIZE code)
{
	System::Call(SYS_EXIT, code);
	__builtin_unreachable();
}
