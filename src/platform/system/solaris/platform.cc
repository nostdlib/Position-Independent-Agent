#include "platform/platform.h"
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"

NO_RETURN VOID ExitProcess(USIZE code)
{
	System::Call(SYS_EXIT, code);
	__builtin_unreachable();
}
