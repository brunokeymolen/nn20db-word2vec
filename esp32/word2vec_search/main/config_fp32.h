#ifndef CONFIG_FP32_H
#define CONFIG_FP32_H

#include <stdio.h>
#include "nn20db_config.h"

#include "net_server.h"

#define DB_PATH         "/sdcard/nand0/w2vfp32"


static const nn20db_config s_nn20db_config = {
    .vector = {
        .type          = NN20DB_DIMENSION_FLOAT32_CONFIG,
        .dimension     = NET_SERVER_DIM,
        .metadata_size = 32,
    },
    .storage = {
        .type = NN20DB_STORAGE_LFS_CONFIG,
        .lfs = {
            .device_path             = DB_PATH,
            .mount_point             = "/sdcard",
            .lane_cache_size_kb      = 16,
            .lane_size_mb            = 512,
            .log_size_mb             = 4,
            .log_index_buckets       = 1024,
            .object_cache_size_bytes = 4096,
            /* FP32 node worst case ≈ 2.42 KB (24B hdr + 5×6B levels
             * + (64+16+8+4+2)×12B edges + 1200B vector + 32B metadata), so
             * 2560 covers every node in one pread. */
            .read_ahead_size_bytes   = 2560,
            /* 512-byte alignment (SD native sector) instead of 4096 cuts the
             * average alignment waste per get from ~2 KB to ~256 B — measured
             * ~4.0 KB transferred per fetch before. */
            .block_size              = 512,
            .flags                   = NN20DB_STORAGE_FLAGS_DISABLE_CRC
                                        | NN20DB_STORAGE_FLAGS_READ_ONLY,
        },
        .cache = {
            .enabled              = 1,
            .max_entries          = 128,   /* 128 × 2560 = 320 KB in S3 PSRAM */
            .max_object_size_bytes = 2560, /* FP32 node size; without this the
                                              slot inherits the 4 KB
                                              object_cache_size_bytes */
        },
    },
    .metric = {
        .type = METRIC_EUCLIDEAN_F32_CONFIG,
    },
    .tuning = {
        /* Same ~4 MB PSRAM budget as the PQ config's node cache: FP32 nodes
         * are ~2x larger, so half the entries. */
        .hnsw_node_cache_capacity = 2048,
        .hnsw_cache_warm_depth    = 2,
    },
};

#endif