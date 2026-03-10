#pragma once

#include "primitives.h"

enum CommandType : UINT8
{
    GetSystemInfo = 0,
    GetDirectoryContent = 1,
    GetFileContent = 2,
    GetFileChunkHash = 3,
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
