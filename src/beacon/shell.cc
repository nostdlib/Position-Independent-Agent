#include "shell.h"

Shell::Shell(ShellProcess &&sp) noexcept
    : shellProcess(static_cast<ShellProcess &&>(sp)) {}

Shell::Shell(Shell &&other) noexcept
    : shellProcess(static_cast<ShellProcess &&>(other.shellProcess)) {}

Result<Shell, Error> Shell::Create() noexcept
{
    auto result = ShellProcess::Create();
    if (!result)
        return Result<Shell, Error>::Err(Error::Process_CreateFailed);

    return Result<Shell, Error>::Ok(
        Shell(static_cast<ShellProcess &&>(result.Value())));
}

Result<USIZE, Error> Shell::Write(const CHAR* data, USIZE length) noexcept
{
    return shellProcess.Write(data, length);
}

Result<USIZE, Error> Shell::Read(PCHAR buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1)
    {
        SSIZE pollRet = shellProcess.Poll((totalRead == 0) ? 5000 : 100);
        if (pollRet <= 0)
            break;

        auto ret = shellProcess.Read(buffer + totalRead, (capacity - 1) - totalRead);
        if (!ret || ret.Value() == 0)
            return Result<USIZE, Error>::Err(Error::ShellProcess_ReadFailed);

        bool promptFound = false;

        for (USIZE i = 0; i < ret.Value(); ++i)
        {
            if (buffer[totalRead + i] == ShellProcess::EndOfLineChar())
            {
                promptFound = true;
                break;
            }
        }

        totalRead += ret.Value();
        if (promptFound)
            break;
    }

    return Result<USIZE, Error>::Ok(totalRead);
}

Result<USIZE, Error> Shell::ReadError(PCHAR buffer, USIZE capacity) noexcept
{
    return shellProcess.ReadError(buffer, capacity);
}
