#pragma once

#include "primitives.h"

enum CommandType : UINT8
{
    Command_GetSystemInfo = 0,
    Command_GetDirectoryContent = 1,
    Command_GetFileContent = 2,
    Command_GetFileChunkHash = 3,
    CommandTypeCount
};

enum StatusCode : UINT32
{
    StatusSuccess = 0,
    StatusError = 1,
    StatusUnknownCommand = 2
};

using CommandHandler = VOID (*)(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength);

VOID Handle_GetSystemInfoCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength);
VOID Handle_GetDirectoryContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength);
VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength);
VOID Handle_GetFileChunkHashCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength);
