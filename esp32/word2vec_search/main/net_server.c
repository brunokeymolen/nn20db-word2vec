/*
 * net_server.c — TCP server for Word2Vec vector search queries
 *
 * Listens on NET_SERVER_PORT (9900).
 * For each connection:
 *   1. Receive uint16 k + uint16 ef_search + 300 × float32 LE.
 *   2. Run nn20db_vector_search_ef with the requested k and ef_search.
 *   3. Fetch word_meta_t for each result.
 *   4. Send k × 36 bytes (32-byte word label + 4-byte float32 distance).
 *   5. Close connection.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "nn20db.h"
#include "nn20db_config.h"

#include "net_server.h"
#include "display.h"

static const char *TAG = "net_server";

#pragma pack(push, 1)
typedef struct {
    char word[32];
} word_meta_t;

typedef struct {
    uint16_t k;
    uint16_t ef_search;
    float    query[NET_SERVER_DIM];
} request_wire_t;

typedef struct {
    char  word[32];
    float distance;
} result_wire_t;
#pragma pack(pop)

_Static_assert(sizeof(request_wire_t) == (4 + (NET_SERVER_DIM * 4)),
               "request_wire_t size");
_Static_assert(sizeof(word_meta_t)   == 32, "word_meta_t size");
_Static_assert(sizeof(result_wire_t) == 36, "result_wire_t size");

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int recv_all(int sock, void *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int n = recv(sock, (char *)buf + received, len - received, 0);
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    return 0;
}

static int send_all(int sock, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── per-connection handler ──────────────────────────────────────────────── */

static void handle_connection(int conn_fd, NN20DB *db) {
    request_wire_t request;
    nn20db_vector_search_result results[NET_SERVER_MAX_TOP_K];
    result_wire_t wire[NET_SERVER_MAX_TOP_K];
    char words[NET_SERVER_MAX_TOP_K][32];
    int requested_k;
    int effective_k;
    int ef_search;

    memset(wire, 0, sizeof(wire));
    memset(words, 0, sizeof(words));

    /* receive request header and query vector */
    if (recv_all(conn_fd, &request, sizeof(request)) != 0) {
        ESP_LOGW(TAG, "recv failed: %d", errno);
        return;
    }

    requested_k = (int)request.k;
    ef_search = (int)request.ef_search;

    if (requested_k <= 0) {
        requested_k = NET_SERVER_DEFAULT_TOP_K;
    }
    effective_k = requested_k;
    if (effective_k > NET_SERVER_MAX_TOP_K) {
        ESP_LOGW(TAG, "clamping k from %d to %d", requested_k, NET_SERVER_MAX_TOP_K);
        effective_k = NET_SERVER_MAX_TOP_K;
    }
    if (ef_search <= 0) {
        ef_search = NET_SERVER_DEFAULT_EF;
    }

    int64_t t0 = esp_timer_get_time();
    display_show_searching();

    int rc = nn20db_vector_search_ef(db, request.query, effective_k,
                                     ef_search, results);
    int64_t search_us = esp_timer_get_time() - t0;

    if (rc != NN20DB_ERROR_OK) {
        ESP_LOGW(TAG, "search failed rc=%d", rc);
        /* send empty response so the client gets something */
        send_all(conn_fd, wire, (size_t)effective_k * sizeof(*wire));
        return;
    }

    /* collect metadata for each result */
    for (int i = 0; i < effective_k; i++) {
        word_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        if (nn20db_vector_get(db, results[i].id, NULL, &meta) == NN20DB_ERROR_OK) {
            memcpy(wire[i].word, meta.word, 32);
        }
        wire[i].distance = results[i].distance;
        memcpy(words[i], wire[i].word, 32);
    }

    /* display results */
    display_show_results(words, effective_k, ef_search, (float)(search_us / 1000.0));

    ESP_LOGI(TAG, "search OK: k=%d ef=%d top1='%.32s' d=%.4f  t=%lldus",
             effective_k, ef_search, wire[0].word,
             (double)wire[0].distance, (long long)search_us);

    /* send response */
    send_all(conn_fd, wire, (size_t)effective_k * sizeof(*wire));
}

/* ── server task ─────────────────────────────────────────────────────────── */

static void net_server_task(void *arg) {
    NN20DB *db = (NN20DB *)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(NET_SERVER_PORT),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening on port %d", NET_SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int conn_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (conn_fd < 0) {
            ESP_LOGW(TAG, "accept failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char client_ip[32];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "Connection from %s", client_ip);

        handle_connection(conn_fd, db);
        close(conn_fd);
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

void net_server_start(NN20DB *db) {
    xTaskCreate(net_server_task, "net_server", 8 * 1024, db,
                tskIDLE_PRIORITY + 2, NULL);
}
