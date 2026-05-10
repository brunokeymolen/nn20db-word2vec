#pragma once
#include <stdint.h>
#include "nn20db.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wire protocol:
 *
 *   Request  (Linux → ESP32):  uint16 k + uint16 ef_search
 *                              + 300 × float32 LE
 *                              = 1204 bytes
 *   Response (ESP32 → Linux):  k × (32-byte word + 4-byte float32)
 *                              = k × 36 bytes
 *
 * The server listens on NET_SERVER_PORT and handles one connection at a time.
 */
#define NET_SERVER_PORT           9900
#define NET_SERVER_DIM            300
#define NET_SERVER_DEFAULT_TOP_K  10
#define NET_SERVER_MAX_TOP_K      32
#define NET_SERVER_DEFAULT_EF     15

/* Start the TCP server task. db must remain valid for the lifetime of the task. */
void net_server_start(NN20DB *db);

#ifdef __cplusplus
}
#endif
