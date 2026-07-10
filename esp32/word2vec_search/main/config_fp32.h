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
            .read_ahead_size_bytes   = 2048,
            .block_size              = 4096,
            .flags                   = NN20DB_STORAGE_FLAGS_DISABLE_CRC 
                                        | NN20DB_STORAGE_FLAGS_READ_ONLY,
        },
        .cache = {
            .enabled     = 1,
            .max_entries = 16,  /* 16 × 4096 = 64 KB — safe for S3 PSRAM */
        },
    },
    .metric = {
        .type = METRIC_EUCLIDEAN_F32_CONFIG,
    },
};

#endif