#ifndef CONFIG_PQ_H
#define CONFIG_PQ_H

#include "nn20db_config.h"
#include <stdio.h>

#define WORD2VEC_DIM        300
#define METADATA_SIZE       32      /* sizeof(word_meta_t) */


static nn20db_config make_config(const char *db_path, int ef_search) {
    nn20db_config cfg = {
        .vector = {
            .type          = NN20DB_DIMENSION_PQ_CONFIG,
            .dimension     = WORD2VEC_DIM,
            .metadata_size = METADATA_SIZE,
            .pq = {
                .num_segments = 20,        /* M: number of subvectors */
                .bits_per_segment = 8,     /* K: 256 codes per segment */
                .subvector_dim = 15,       /* 300 / 20 = 15D per subvector */
            }
        },
        .storage = {
            .type = NN20DB_STORAGE_LFS_CONFIG,
            .lfs = {
                .lane_cache_size_kb      = 8192,
                .lane_size_mb            = 256,
                .log_size_mb             = 500,
                .log_index_buckets       = 262144,
                .object_cache_size_bytes = 4096,
                .read_ahead_size_bytes   = 4096,
                .block_size              = 4096,
                .flags                   = NN20DB_STORAGE_FLAGS_DISABLE_CRC,
          },
            .cache = {
                .enabled     = 1,
                .max_entries = 1048576 * 3,
            },
        },
        .metric = {
            .type = METRIC_EUCLIDEAN_PQ_CONFIG,
        },
        .index = {
            .type = NN20DB_INDEX_HNSW_CONFIG,
            .hnsw = {
                .search_threads          = 1,
                .max_levels              = 5,
                .diversity_alpha         = 1.2f,
                .search_seen_set_capacity = 20000,
                .ef_search               = 0, /* set below */
                .level_config = {
                    [0] = { .M = 32, .ef_construction = 250 }, 
                    [1] = { .M = 16, .ef_construction = 120 },
                    [2] = { .M =  8, .ef_construction =  60 },
                    [3] = { .M =  4, .ef_construction =  30 },
                    [4] = { .M =  2, .ef_construction =  15 },
                },
            },
        },
    };

    snprintf(cfg.storage.lfs.device_path,
             sizeof(cfg.storage.lfs.device_path), "%s", db_path);
    cfg.index.hnsw.ef_search = (uint16_t)ef_search;

    return cfg;
}
#endif