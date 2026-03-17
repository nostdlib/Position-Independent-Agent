#pragma once

#include "runtime.h"

class Shell
{
private:
    Process process;

#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_UEFI)
    Pipe stdinPipe;
    Pipe stdoutPipe;
    Pipe stderrPipe;
    Shell(Process &&proc, Pipe &&inP, Pipe &&outP, Pipe &&errP) noexcept;
#else
    SSIZE masterFd;
    Shell(Process &&proc, SSIZE master) noexcept;
#endif

public:
#if defined(PLATFORM_WINDOWS)
    constexpr static char EndOfLineChar = '>';
#else
    constexpr static char EndOfLineChar = '$';
#endif

    static Result<Shell, Error> Create() noexcept;
    Result<USIZE, Error> Write(const char *data, USIZE length) noexcept;
    Result<USIZE, Error> Read(char *buffer, USIZE capacity) noexcept;
    Result<USIZE, Error> ReadError(char *buffer, USIZE capacity) noexcept;

    ~Shell() noexcept;
    Shell(Shell &&other) noexcept;
    Shell &operator=(Shell &&) = delete;
    Shell(const Shell &) = delete;
    Shell &operator=(const Shell &) = delete;
};
