
#include "shell.h"

Shell::Shell(Process &&proc, Pipe &&inP, Pipe &&outP, Pipe &&errP) noexcept
    : process(static_cast<Process &&>(proc)),
      stdinPipe(static_cast<Pipe &&>(inP)),
      stdoutPipe(static_cast<Pipe &&>(outP)),
      stderrPipe(static_cast<Pipe &&>(errP)) {
      };

/**
 * Sends a string to the shell's stdin.
 * Note: You usually need to include a '\n' at the end of the string.
 */
Result<USIZE, Error> Shell::Write(const char *data, USIZE length) noexcept
{
    return stdinPipe.Write(Span<const UINT8>((const UINT8 *)data, length));
}

/**
 * Reads available output from the shell's stdout.
 */
Result<USIZE, Error> Shell::Read(char *buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1) // Leave room for null terminator if needed
    {
        char byte;
        // Read 1 byte at a time
        auto result = stdoutPipe.Read(Span<UINT8>((UINT8 *)&byte, 1));

        if (!result)
        {
            return Result<USIZE, Error>::Err(result.Error());
        }

        USIZE bytesRead = result.Value();
        if (bytesRead == 0)
            break; // EOF reached

        buffer[totalRead++] = byte;

        // Check if we hit the shell prompt ('>' on Windows, '$' on Linux)
        if (byte == Shell::EndOfLineChar)
        {
            break;
        }
    }

    return Result<USIZE, Error>::Ok(totalRead);
}

/**
 * Reads available error messages from the shell's stderr.
 */
Result<USIZE, Error> Shell::ReadError(char *buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1) // Leave room for null terminator if needed
    {
        char byte;
        // Read 1 byte at a time
        auto result = stderrPipe.Read(Span<UINT8>((UINT8 *)&byte, 1));

        if (!result)
        {
            return Result<USIZE, Error>::Err(result.Error());
        }

        USIZE bytesRead = result.Value();
        if (bytesRead == 0)
            break; // EOF reached

        buffer[totalRead++] = byte;

        // Check if we hit the shell prompt ('>' on Windows, '$' on Linux)
        if (byte == EndOfLineChar)
        {
            break;
        }
    }

    return Result<USIZE, Error>::Ok(totalRead);
}

// --- Factory Method ---

Result<Shell, Error> Shell::Create() noexcept
{
    auto stdinResult = Pipe::Create();
    auto stdoutResult = Pipe::Create();
    auto stderrResult = Pipe::Create();

    if (!stdinResult || !stdoutResult || !stderrResult)
    {
        return Result<Shell, Error>::Err(Error::Pipe_CreateFailed);
    }

    auto stdinPipe = static_cast<Pipe &&>(stdinResult.Value());
    auto stdoutPipe = static_cast<Pipe &&>(stdoutResult.Value());
    auto stderrPipe = static_cast<Pipe &&>(stderrResult.Value());

#if defined(PLATFORM_WINDOWS)
    auto path = "cmd.exe"_embed;
#else
    auto path = "/bin/sh"_embed;
#endif

    auto processResult = Process::Create(
        path,
        nullptr,
        stdinPipe.ReadEnd(),
        stdoutPipe.WriteEnd(),
        stderrPipe.WriteEnd());

    if (!processResult)
    {
        return Result<Shell, Error>::Err(Error::Process_CreateFailed);
    }

    // // Close parent's access to the child's ends of the pipes
    // stdinPipe.CloseReadEnd();
    // stdoutPipe.CloseWriteEnd();
    // stderrPipe.CloseWriteEnd();

    return Result<Shell, Error>::Ok(
        Shell(
            static_cast<Process &&>(processResult.Value()),
            static_cast<Pipe &&>(stdinPipe),
            static_cast<Pipe &&>(stdoutPipe),
            static_cast<Pipe &&>(stderrPipe)));
}