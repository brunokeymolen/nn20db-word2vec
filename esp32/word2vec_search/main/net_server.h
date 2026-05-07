#pragma once
#include <stdint.h>
#include "nn20db.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wire protocol (phase 1):
 *
 *   Request  (Linux → ESP32):  300 × float32 LE  =  1200 bytes
 *   Response (ESP32 → Linux):  k × (32-byte word + 4-byte float32)
 *                              =  k × 36 bytes  (k = NET_SERVER_TOP_K)
 *
 * The server listens on NET_SERVER_PORT and handles one connection at a time.
 */
#define NET_SERVER_PORT    9900
#define NET_SERVER_DIM     300
#define NET_SERVER_TOP_K   10
#define NET_SERVER_EF      32    /* ef_search on the device */

/* Start the TCP server task. db must remain valid for the lifetime of the task. */
void net_server_start(NN20DB *db);

#ifdef __cplusplus
}
#endif
