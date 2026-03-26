# Networking: DNS, TLS, HTTP, and WebSocket From Scratch

This document covers the full network stack: DNS-over-HTTPS resolution, TLS 1.3
handshake and encryption, raw HTTP/1.1 wire format, and WebSocket framing. Every layer
is built from scratch -- no libc, no OpenSSL, no system resolver.

For the custom types (`Result`, `Span`, `Error`) used throughout, see
[docs/05-core-types.md](05-core-types.md). For memory primitives, see
[docs/06-memory-and-strings.md](06-memory-and-strings.md).

---

## 1. The Full Connection Chain

```
 Layer 5: WebSocket         websocket_client.h    Binary frames (commands, responses)
            |
 Layer 4: HTTP/1.1          http_client.h         GET /agent HTTP/1.1 + Upgrade: websocket
            |
 Layer 3: TLS 1.3           tls_client.h          Encrypted channel (ChaCha20-Poly1305)
            |
 Layer 2: TCP Socket        socket.h              connect() to IP:443
            |
 Layer 1: DNS Resolution    dns_client.h          "relay.nostdlib.workers.dev" -> IP
```

Each layer wraps the one below it. The sequence for a single connection: DNS resolves
the hostname, TCP opens the socket, TLS negotiates encryption, HTTP sends the Upgrade
request, WebSocket takes over for bidirectional binary messaging.

---

## 2. DNS-over-HTTPS (DoH)

**Source:** `src/lib/network/dns/dns_client.h`

Regular DNS sends plaintext UDP to port 53. DoH wraps the same RFC 1035 wire-format
query inside an HTTPS POST, making it indistinguishable from normal web traffic:

```
POST /dns-query HTTP/1.1
Host: one.one.one.one
Content-Type: application/dns-message
Content-Length: <length>
<raw DNS wire-format query bytes>
```

The core method opens a TLS 1.3 connection to the DoH server and sends the query per
RFC 8484 Section 4.1:

```cpp
[[nodiscard]] static Result<IPAddress, Error> ResolveOverHttp(
    Span<const CHAR> host, const IPAddress &dnsServerIp,
    Span<const CHAR> dnsServerName, DnsRecordType dnstype = DnsRecordType::AAAA);
```

### DNS Fallback Chain

```cpp
template <USIZE N>
[[nodiscard]] static Result<IPAddress, Error> ResolveWithFallback(
    Span<const CHAR> host, Span<const IPAddress, N> ips,
    Span<const CHAR> serverName, DnsRecordType dnstype)
{
    for (USIZE i = 0; i < N; i++) {
        auto result = ResolveOverHttp(host, ips[i], serverName, dnstype);
        if (result) return result;
    }
    return Result<IPAddress, Error>::Err(Error::Dns_ResolveFailed);
}
```

Provider order: Cloudflare 1.1.1.1, then 1.0.0.1, then Google 8.8.8.8, then 8.8.4.4.
If AAAA (IPv6) fails across all four, the entire chain retries with A (IPv4). If
everything fails, the beacon sleeps and retries -- total failure usually means an
air-gapped host or a firewall blocking HTTPS to known DNS provider IPs.

---

## 3. TLS 1.3 Handshake

**Source:** `src/lib/network/tls/tls_client.h`, `tls_client.cc`

### Sequence Diagram

```
Client                                         Server
  |                                              |
  |-------- ClientHello ------------------------>|
  |         - Supported ciphers                  |
  |         - Client random (32 bytes)           |
  |         - ECDH public keys (P-256, P-384)    |
  |         - SNI (server hostname)              |
  |                                              |
  |<------- ServerHello -------------------------|
  |         - Chosen cipher (ChaCha20-Poly1305)  |
  |         - Server ECDH public key             |
  |                                              |
  |         [Both compute shared secret]         |
  |         [Derive handshake traffic keys]      |
  |                                              |
  |<------- {EncryptedExtensions} ---------------|
  |<------- {Certificate} -----------------------|
  |<------- {CertificateVerify} -----------------|
  |<------- {Finished} --------------------------|
  |                                              |
  |-------- {Finished} ------------------------->|
  |                                              |
  |========= Encrypted application data =========|
```

Messages in `{}` are encrypted. One round trip total (down from two in TLS 1.2). Only
one cipher suite: `TLS_CHACHA20_POLY1305_SHA256` (0x1303). ECDH uses P-256 or P-384.

### TLS State Machine

The handshake is driven by `stateIndex` and a table of expected messages:

```cpp
TlsState state_seq[6]{};
state_seq[0] = {CONTENT_HANDSHAKE, MSG_SERVER_HELLO};
state_seq[1] = {CONTENT_CHANGECIPHERSPEC, MSG_CHANGE_CIPHER_SPEC};
state_seq[2] = {CONTENT_HANDSHAKE, MSG_ENCRYPTED_EXTENSIONS};
state_seq[3] = {CONTENT_HANDSHAKE, MSG_CERTIFICATE};
state_seq[4] = {CONTENT_HANDSHAKE, MSG_CERTIFICATE_VERIFY};
state_seq[5] = {CONTENT_HANDSHAKE, MSG_FINISHED};
```

```
 [ServerHello] -> [ChangeCipherSpec] -> [EncryptedExtensions]
                                               |
 [DONE] <- [ClientFinished] <- [Finished] <- [CertVerify] <- [Certificate]
```

Each incoming packet is checked against `state_seq[stateIndex]`. Mismatch aborts with
`Error::Tls_OnPacketFailed`. `Open()` loops `while (stateIndex < 6)`, then sends client
Finished. After completion, `Read()` and `Write()` transparently encrypt/decrypt.

---

## 4. HKDF Key Derivation

**Source:** `src/lib/network/tls/tls_hkdf.h`, `tls_hkdf.cc`

After ECDH, both sides share one secret. TLS 1.3 needs six keys. HKDF (RFC 5869) bridges
the gap in two phases:

**Extract** -- concentrates entropy into a fixed-size PRK:

```cpp
VOID TlsHKDF::Extract(Span<UCHAR> output, Span<const UCHAR> salt, Span<const UCHAR> ikm)
{
    HMAC_SHA256 hmac;
    hmac.Init(salt);
    hmac.Update(ikm);
    hmac.Final(output);  // PRK = HMAC-SHA256(salt, IKM)
}
```

**Expand** -- stretches the PRK into multiple keys using labeled info:

```cpp
VOID TlsHKDF::ExpandLabel(Span<UCHAR> output, Span<const UCHAR> secret,
    Span<const CHAR> label, Span<const UCHAR> data);
```

Labels are prefixed with `"tls13 "`. By varying the label ("key", "iv", "finished",
"derived"), TLS 1.3 derives client write key, server write key, client IV, server IV,
and handshake traffic keys -- all from one ECDH shared secret. The derived keys live in
`TlsCipher::data13` (`mainSecret`, `handshakeSecret`, `pseudoRandomKey`), and feed the
`ChaCha20Encoder` for record-level encryption. For the SHA-256/HMAC primitives, see
`src/lib/crypto/sha2.h`.

---

## 5. Raw HTTP/1.1 Wire Format

**Source:** `src/lib/network/http/http_client.h`

What actually goes over the TLS channel:

```
Request:                              Response:
  GET /agent HTTP/1.1\r\n              HTTP/1.1 101 Switching Protocols\r\n
  Host: relay.nostdlib.workers.dev\r\n Upgrade: websocket\r\n
  Connection: Upgrade\r\n             Connection: Upgrade\r\n
  Upgrade: websocket\r\n              Sec-WebSocket-Accept: s3pPLM...\r\n
  Sec-WebSocket-Key: dGhlIH...\r\n    \r\n
  Sec-WebSocket-Version: 13\r\n
  \r\n
```

Every line ends with `\r\n`. Headers end with a blank `\r\n\r\n`. Status 101 means the
protocol switch succeeded -- from here, the connection speaks WebSocket.

`HttpClient::Create()` parses the URL, resolves via `DnsClient::Resolve()`, and
constructs the `TlsClient`. `ParseUrl()` handles `http://`, `https://`, `ws://`, `wss://`
schemes, defaulting to port 80 (plaintext) or 443 (secure).

---

## 6. Rolling Window Header Detection

**Source:** `src/lib/network/http/http_client.h` lines 192-208

`ReadResponseHeaders` needs to find `\r\n\r\n` (0x0D 0x0A 0x0D 0x0A) marking end of
headers. It reads one byte at a time with a 4-byte sliding window:

```
Byte stream:  ... o s t : \r \n \r \n ...
Window:       [o,s,t,:]  no match
              [s,t,:,\r]  no match
              [\r,\n,\r,\n]  MATCH
```

This works even when `\r\n\r\n` splits across TCP reads. The method also validates the
status code at byte offset 9-12 and extracts `Content-Length` via a character-by-character
state machine. Headers exceeding 16 KiB cause an abort.

---

## 7. WebSocket vs HTTP

HTTP is request/response -- the server cannot push commands without the client polling.
WebSocket starts as an HTTP Upgrade, then switches to bidirectional binary framing.

| Property         | HTTP/1.1              | WebSocket                   |
|------------------|-----------------------|-----------------------------|
| Direction        | Client-initiated      | Bidirectional               |
| Framing          | Text headers + body   | Binary frames with opcodes  |
| Per-msg overhead | ~200 bytes headers    | 2-14 bytes frame header     |
| Server push      | Not possible          | Native                      |

```cpp
enum class WebSocketOpcode : UINT8 {
    Continue = 0x0,  Text = 0x1,  Binary = 0x2,
    Close    = 0x8,  Ping = 0x9,  Pong   = 0xA
};
```

The agent uses `Binary` frames. Control frames are handled transparently -- Ping triggers
an automatic Pong, Close echoes the status code and shuts down.

---

## 8. WebSocket Frame Format and Masking

```
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------+
|F|R|R|R| opcode|M| Payload len | Extended len|
|I|S|S|S|  (4)  |A|     (7)     |  (16 or 64) |
|N|V|V|V|       |S|             |             |
+-+-+-+-+-------+-+-------------+-------------+
| Masking-key (4 bytes, if MASK=1)            |
+---------------------------------------------+
| Payload Data ...                            |
+---------------------------------------------+
```

Payload length: 0-125 inline, 126 means 16-bit follows, 127 means 64-bit follows.
Frames >64 MB are rejected. The `WebSocketFrame` struct maps directly to this wire format.

### Frame Masking

Client-to-server frames MUST be masked. Server-to-client MUST NOT. The algorithm:

```
masked[i] = payload[i] ^ masking_key[i % 4]
```

The key is in the frame header -- not a secret. The purpose: prevent cache poisoning
against HTTP proxies. Without masking, a crafted WebSocket payload could look like a
valid HTTP response to an intermediary, poisoning its cache. Masking makes payload bytes
look random, defeating the attack. XOR is self-inverse, so `MaskFrame` both masks and
unmasks:

```cpp
static VOID MaskFrame(WebSocketFrame &frame, UINT32 maskKey);
```

The `Write()` method optimizes by batching header + masked payload into one TLS write
for small frames (<=242 bytes), and streaming 256-byte masked chunks for large ones.

---

## 9. Full Connection Trace

```
WebSocketClient::Create("wss://relay.nostdlib.workers.dev/agent")
  |
  +-- HttpClient::ParseUrl()     -> host, path, port=443, secure=true
  +-- DnsClient::Resolve()       -> AAAA via Cloudflare, fallback Google, fallback A
  |     +-- ResolveOverHttp()    -> TLS to DoH server, POST /dns-query
  +-- TlsClient::Create()       -> Create context for relay server
  +-- Open()
        +-- TlsClient::Open()   -> TCP connect + TLS 1.3 handshake (state machine)
        +-- Send HTTP Upgrade    -> GET /agent + WebSocket headers
        +-- ReadResponseHeaders()-> Expect 101, rolling window detection
        +-- [WebSocket OPEN]     -> Ready for Read()/Write()
```

Note the recursion: resolving a hostname requires a TLS connection to the DoH server.
That inner connection uses hardcoded IPs (1.1.1.1, etc.), breaking the recursion.

---

## Cross-References

- **Platform sockets:** `src/platform/socket/socket.h` -- TCP over Linux syscalls,
  Windows AFD, macOS BSD, UEFI TCP protocols
- **ChaCha20-Poly1305:** `src/lib/crypto/chacha20_encoder.h` -- AEAD cipher for TLS
- **SHA-256/HMAC:** `src/lib/crypto/sha2.h` -- hash primitives for HKDF and transcripts
- **ECC:** `src/lib/crypto/ecc.h` -- P-256/P-384 for ECDH key exchange
- **Error handling:** `src/core/types/error.h` -- `Result<T, Error>` used everywhere
- **Architecture:** [docs/ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) -- module graph
