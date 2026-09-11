/**
 * @file runtime.h
 * @brief Runtime Abstraction Layer
 *
 * @details Aggregate header for the RUNTIME layer. Includes CORE + PLATFORM
 * (via platform.h) plus all runtime-level features: cryptography, networking,
 * and TLS 1.3.
 *
 * @defgroup runtime Runtime Abstraction Layer
 * @{
 */

#pragma once

#include "platform/platform.h"

#include "core/containers/vector.h"
#include "core/containers/buffer.h"
#include "core/containers/bitset.h"
#include "core/containers/byte_queue.h"

#include "lib/crypto/sha2.h"
#include "lib/crypto/ecc.h"
#include "lib/crypto/chacha20.h"
#include "lib/crypto/chacha20_encoder.h"

#include "lib/network/dns/dns_client.h"
#include "lib/network/http/http_client.h"
#include "lib/network/websocket/websocket_client.h"

#include "lib/image/jpeg_encoder.h"
#include "lib/image/image_processor.h"

#include "lib/network/tls/tls_client.h"
#include "lib/network/tls/tls_buffer.h"
#include "lib/network/tls/tls_cipher.h"
#include "lib/network/tls/tls_hash.h"
#include "lib/network/tls/tls_hkdf.h"


/** @} */ // end of runtime group
