#pragma once

#include "runtime.h"

class Shell
{
private:
    ShellProcess shellProcess;
    Shell(ShellProcess &&sp) noexcept;

public:
    static Result<Shell, Error> Create() noexcept;
    Result<USIZE, Error> Write(const CHAR* data, USIZE length) noexcept;
    Result<USIZE, Error> Read(PCHAR buffer, USIZE capacity) noexcept;
    Result<USIZE, Error> ReadError(PCHAR buffer, USIZE capacity) noexcept;

    ~Shell() noexcept = default;
    Shell(Shell &&other) noexcept;
    Shell &operator=(Shell &&) = delete;
    Shell(const Shell &) = delete;
    Shell &operator=(const Shell &) = delete;
};
