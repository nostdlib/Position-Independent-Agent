#pragma once

#include "runtime.h"
class Shell
{
private:
    Process process;
    Pipe stdinPipe;
    Pipe stdoutPipe;
    Pipe stderrPipe;

    // Private constructor to enforce factory pattern
    Shell(Process &&proc, Pipe &&inP, Pipe &&outP, Pipe &&errP) noexcept;

public:
#if defined(PLATFORM_WINDOWS)
    constexpr static char EndOfLineChar = '>';
#else
    constexpr static char EndOfLineChar = '$';
#endif

    /**
     * Creates a new Shell instance by spawning a child process and setting up pipes.
     */
    static Result<Shell, Error> Create() noexcept;

    /**
     * Sends a command to the shell.
     */
    Result<USIZE, Error> Write(const char *data, USIZE length) noexcept;

    /**
     * Reads a single chunk of data from stdout.
     */
    Result<USIZE, Error> Read(char *buffer, USIZE capacity) noexcept;

    /**
     * Reads from stderr.
     */
    Result<USIZE, Error> ReadError(char *buffer, USIZE capacity) noexcept;
};