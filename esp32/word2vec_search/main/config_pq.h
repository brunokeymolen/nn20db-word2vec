#ifndef CONFIG_PQ_H
#define CONFIG_PQ_H

#include <stdio.h>
#include "nn20db_config.h"

#include "net_server.h"

#define DB_PATH         "/sdcard/nand0/w2vpq"
//#define DB_PATH         "/sdcard/nand0/w2v3mpq"

static const nn20db_config s_nn20db_config = {
    .vector = {
        .type          = NN20DB_DIMENSION_PQ_CONFIG,
        .dimension     = NET_SERVER_DIM,
        .metadata_size = 32,
        .pq = {
            .num_segments = 20,        /* M: number of subvectors */
            .bits_per_segment = 8,     /* K: 256 codes per segment */
            .subvector_dim = 15,       /* 300 / 20 = 15D per subvector */
        }
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
            /* PQ node worst case ≈ 1.25 KB (5 levels: 24B hdr + 5×6B levels
             * + (64+16+8+4+2)×12B edges + 20B codes + 32B metadata), so
             * 1536 covers every node in one pread. */
            .read_ahead_size_bytes   = 1536,
            /* SD cards transfer in 512-byte sectors; aligning reads to 512
             * instead of 4096 cuts the average alignment waste per get from
             * ~2 KB to ~256 B. */
            .block_size              = 512,
            /* READ_ONLY opens lanes O_RDONLY, which is what lets IDF's FATFS
             * build the fast-seek cluster map (CONFIG_FATFS_USE_FASTSEEK).
             * Without it every random read walks the FAT chain of the 512 MB
             * lane file from the start — dozens of 512 B FAT sector reads
             * per node fetch. Phase 1 is search-only, so no writes needed. */
            .flags                   = NN20DB_STORAGE_FLAGS_DISABLE_CRC 
                                      | NN20DB_STORAGE_FLAGS_READ_ONLY,
        },
        .cache = {
            .enabled              = 1,
            .max_entries          = 128,  /* 128 × 1536 = 192 KB in S3 PSRAM */
            .max_object_size_bytes = 1536, /* PQ node size; without this the
                                              slot inherits the 4 KB
                                              object_cache_size_bytes */
        },
    },
    .metric = {
        .type = METRIC_EUCLIDEAN_PQ_CONFIG,
    },
    .tuning = {
        /* Warm-up BFS around the EP at L0 with M=32 (64 edge slots) touches
         * up to ~4.2K nodes at depth 2; the old hardcoded 512-node cache
         * dropped 7/8 of them. 4096 × ~1 KB PQ nodes ≈ 4 MB PSRAM.
         * Upper-level nodes (hit by every search descent) displace L0-only
         * entries once full. */
        .hnsw_node_cache_capacity = 4096,
        .hnsw_cache_warm_depth    = 2,
    },
};

#endif