/*
 * nn20db-embedded-word2vect
 *
 * build_word2vec_index — Load GoogleNews Word2Vec vectors, normalize to unit
 * length, and build a persistent nn20db HNSW index.
 *
 * Usage:
 *   ./build_word2vec_index <input.bin.gz> <db_path> [--limit N] [--ef-search N]
 *
 * On first run the database is created and all vectors are inserted.
 * On subsequent runs the existing database is opened and a recall self-test
 * is executed instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <zlib.h>

#include "nn20db.h"
#include "nn20db_config.h"

/* ── constants ─────────────────────────────────────────────────────────────── */

#define WORD2VEC_DIM        300
#define METADATA_SIZE       32      /* sizeof(word_meta_t) */
#define DEFAULT_EF_SEARCH   100
#define SELF_TEST_QUERIES   200     /* queries used for recall self-test */
#define SELF_TEST_K         10
#define COMPACT_INTERVAL    10000   /* compact + sync every N insertions */

/* ── types ──────────────────────────────────────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    char word[32];   /* null-padded word label */
} word_meta_t;
#pragma pack(pop)

_Static_assert(sizeof(word_meta_t) == 32, "word_meta_t must be 32 bytes");

/* ── vector helpers ─────────────────────────────────────────────────────────── */

static void normalize_l2(float *vec, int dim) {
    double sum = 0.0;
    for (int i = 0; i < dim; i++) {
        sum += (double)vec[i] * (double)vec[i];
    }
    if (sum > 0.0) {
        float inv = (float)(1.0 / sqrt(sum));
        for (int i = 0; i < dim; i++) {
            vec[i] *= inv;
        }
    }
}

/* ── nn20db config ──────────────────────────────────────────────────────────── */

static nn20db_config make_config(const char *db_path, int ef_search) {
    nn20db_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.vector.type          = NN20DB_DIMENSION_FLOAT32_CONFIG;
    cfg.vector.dimension     = WORD2VEC_DIM;
    cfg.vector.metadata_size = METADATA_SIZE;

    cfg.storage.type = NN20DB_STORAGE_LFS_CONFIG;
    snprintf(cfg.storage.lfs.device_path,
             sizeof(cfg.storage.lfs.device_path), "%s", db_path);
    cfg.storage.lfs.lane_cache_size_kb      = 16;
    cfg.storage.lfs.lane_size_mb            = 512;
    cfg.storage.lfs.log_size_mb             = 4;
    cfg.storage.lfs.log_index_buckets       = 1024;
    cfg.storage.lfs.object_cache_size_bytes = 4096;
    cfg.storage.lfs.read_ahead_size_bytes   = 4096;
    cfg.storage.lfs.block_size              = 4096;
    cfg.storage.lfs.flags                   = 0; /* keep CRC on Linux */

    cfg.storage.cache.enabled     = 1;
    cfg.storage.cache.max_entries = 512; /* 512 × 4096 = 2 MB arena */

    cfg.metric.type = METRIC_EUCLIDEAN_F32_CONFIG;

    cfg.index.type                               = NN20DB_INDEX_HNSW_CONFIG;
    cfg.index.hnsw.search_threads               = 1;
    cfg.index.hnsw.search_seen_set_capacity      = 50000;
    cfg.index.hnsw.ef_search                     = (uint16_t)ef_search;
    cfg.index.hnsw.level_config[0].M             = 32;
    cfg.index.hnsw.level_config[0].ef_construction = 400;
    cfg.index.hnsw.level_config[1].M             = 16;
    cfg.index.hnsw.level_config[1].ef_construction = 120;
    cfg.index.hnsw.level_config[2].M             = 8;
    cfg.index.hnsw.level_config[2].ef_construction = 60;
    cfg.index.hnsw.level_config[3].M             = 4;
    cfg.index.hnsw.level_config[3].ef_construction = 30;
    cfg.index.hnsw.level_config[4].M             = 2;
    cfg.index.hnsw.level_config[4].ef_construction = 15;

    return cfg;
}

/* ── open or create DB ──────────────────────────────────────────────────────── */

static NN20DB *open_or_create_db(const nn20db_config *cfg, int *created) {
    NN20DB *db = NULL;
    int rc = nn20db_create(cfg, &db);
    if (rc == NN20DB_ERROR_OK) {
        *created = 1;
        return db;
    }
    rc = nn20db_open_with_config(cfg, &db);
    if (rc == NN20DB_ERROR_OK) {
        *created = 0;
        return db;
    }
    fprintf(stderr, "Failed to open/create DB at '%s' (rc=%d)\n",
            cfg->storage.lfs.device_path, rc);
    return NULL;
}

/* ── timer helper ───────────────────────────────────────────────────────────── */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── Word2Vec binary parser ─────────────────────────────────────────────────── */

/*
 * GoogleNews binary format:
 *   "<count> <dim>\n"
 *   For each word:
 *     <word_bytes> SP <dim × float32 native-endian>
 */
static int ingest_word2vec(NN20DB *db,
                           const char *gz_path,
                           long limit,
                           long *inserted_out)
{
    gzFile f = gzopen(gz_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", gz_path);
        return -1;
    }

    /* read header */
    char header[256];
    if (!gzgets(f, header, sizeof(header))) {
        fprintf(stderr, "Failed to read header\n");
        gzclose(f);
        return -1;
    }
    long total_words, dim;
    if (sscanf(header, "%ld %ld", &total_words, &dim) != 2) {
        fprintf(stderr, "Bad header: '%s'\n", header);
        gzclose(f);
        return -1;
    }
    if (dim != WORD2VEC_DIM) {
        fprintf(stderr, "Unexpected dimension %ld (expected %d)\n", dim, WORD2VEC_DIM);
        gzclose(f);
        return -1;
    }

    printf("Word2Vec file: %ld words, %ld dimensions\n", total_words, dim);
    if (limit > 0 && limit < total_words) {
        printf("Limiting ingest to %ld words\n", limit);
        total_words = limit;
    }

    float *vec = malloc((size_t)dim * sizeof(float));
    if (!vec) {
        gzclose(f);
        return -1;
    }

    double t_start = now_seconds();
    long inserted = 0;
    int rc_overall = 0;

    for (long wi = 0; wi < total_words; wi++) {
        /* read word (terminated by space) */
        char wordbuf[512];
        int wlen = 0;
        int c;
        while ((c = gzgetc(f)) != EOF && c != ' ' && c != '\n') {
            if (wlen < (int)(sizeof(wordbuf) - 1)) {
                wordbuf[wlen++] = (char)c;
            }
        }
        wordbuf[wlen] = '\0';

        if (wlen == 0) {
            /* skip blank / newline-only entries */
            gzread(f, vec, (unsigned)(dim * sizeof(float)));
            continue;
        }

        /* read raw floats */
        int nread = gzread(f, vec, (unsigned)(dim * sizeof(float)));
        if (nread != (int)(dim * sizeof(float))) {
            fprintf(stderr, "\nShort read at word %ld (%s), got %d bytes\n",
                    wi, wordbuf, nread);
            rc_overall = -1;
            break;
        }

        normalize_l2(vec, (int)dim);

        word_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.word, wordbuf, sizeof(meta.word) - 1);

        int rc = nn20db_vector_add(db, vec, &meta);
        if (rc != NN20DB_ERROR_OK) {
            fprintf(stderr, "\nFailed to insert '%s' (rc=%d)\n", wordbuf, rc);
            rc_overall = -1;
            break;
        }

        inserted++;

        if (inserted % COMPACT_INTERVAL == 0) {
            int rc2 = nn20db_compact(db);
            int rc3 = nn20db_sync(db);
            if (rc2 != NN20DB_ERROR_OK || rc3 != NN20DB_ERROR_OK) {
                fprintf(stderr, "\nMaintenance failed after %ld vectors\n", inserted);
            }
        }

        if (inserted % 10000 == 0) {
            double elapsed = now_seconds() - t_start;
            double rate = (elapsed > 0) ? (double)inserted / elapsed : 0.0;
            printf("\r  %ld / %ld  (%.0f vec/s)   ", inserted, total_words, rate);
            fflush(stdout);
        }
    }

    printf("\r  done: inserted %ld vectors in %.1f s\n",
           inserted, now_seconds() - t_start);

    /* final compact + sync */
    if (rc_overall == 0) {
        if (nn20db_compact(db) != NN20DB_ERROR_OK ||
            nn20db_sync(db)    != NN20DB_ERROR_OK) {
            fprintf(stderr, "Final maintenance failed\n");
        }
    }

    free(vec);
    gzclose(f);
    *inserted_out = inserted;
    return rc_overall;
}

/* ── self-test: sample random query words, check recall ────────────────────── */

/*
 * For each sampled vector we search top-K and check that the nearest neighbour
 * (rank 1) has distance ≈ 0 (i.e. the query word is its own nearest neighbour).
 * This validates both the graph quality and the metadata round-trip.
 */
static void run_self_test(NN20DB *db, int ef_search) {
    printf("\n── Self-test: recall@%d over %d random queries ──\n",
           SELF_TEST_K, SELF_TEST_QUERIES);

    nn20db_vector_search_result *results =
        malloc((size_t)SELF_TEST_K * sizeof(*results));
    word_meta_t *meta_buf = malloc((size_t)SELF_TEST_K * sizeof(*meta_buf));
    float *qvec = malloc(WORD2VEC_DIM * sizeof(float));

    if (!results || !meta_buf || !qvec) {
        fprintf(stderr, "Self-test: allocation failed\n");
        free(results); free(meta_buf); free(qvec);
        return;
    }

    /*
     * We sample by fetching sequential IDs starting from a random offset.
     * IDs in nn20db are assigned sequentially from 1, so we iterate 1..N
     * but skip most to get SELF_TEST_QUERIES samples spread across the DB.
     */
    /* Determine approximate DB size by trying to fetch a high ID */
    long n_vectors = 0;
    {
        /* Binary search: find highest valid ID */
        uint64_t lo = 1, hi = 5000000ULL;
        word_meta_t tmp;
        float tmpvec[WORD2VEC_DIM];
        while (lo < hi) {
            uint64_t mid = (lo + hi + 1) / 2;
            if (nn20db_vector_get(db, mid, tmpvec, &tmp) == NN20DB_ERROR_OK) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        n_vectors = (long)lo;
    }
    printf("  DB size ≈ %ld vectors\n", n_vectors);

    if (n_vectors < SELF_TEST_QUERIES) {
        printf("  DB too small for self-test\n");
        free(results); free(meta_buf); free(qvec);
        return;
    }

    long step = n_vectors / SELF_TEST_QUERIES;
    int hits = 0;
    double total_search_ms = 0.0;

    for (int qi = 0; qi < SELF_TEST_QUERIES; qi++) {
        uint64_t qid = (uint64_t)(1 + qi * step);
        word_meta_t qmeta;

        if (nn20db_vector_get(db, qid, qvec, &qmeta) != NN20DB_ERROR_OK) {
            continue;
        }

        double t0 = now_seconds();
        int rc = nn20db_vector_search_ef(db, qvec, SELF_TEST_K, ef_search, results);
        total_search_ms += (now_seconds() - t0) * 1000.0;

        if (rc != NN20DB_ERROR_OK) continue;

        /* fetch metadata for results and check if rank-1 distance ≈ 0 */
        for (int ri = 0; ri < SELF_TEST_K; ri++) {
            if (nn20db_vector_get(db, results[ri].id, NULL, &meta_buf[ri])
                    != NN20DB_ERROR_OK) {
                memset(&meta_buf[ri], 0, sizeof(meta_buf[ri]));
            }
        }

        /* hit: query word appears anywhere in top-k */
        for (int ri = 0; ri < SELF_TEST_K; ri++) {
            if (strncmp(qmeta.word, meta_buf[ri].word, 32) == 0) {
                hits++;
                break;
            }
        }
    }

    double recall = (double)hits / (double)SELF_TEST_QUERIES;
    double avg_ms = total_search_ms / (double)SELF_TEST_QUERIES;
    printf("  recall@%d = %.4f  (%d/%d)  avg_search=%.3f ms  ef_search=%d\n",
           SELF_TEST_K, recall, hits, SELF_TEST_QUERIES, avg_ms, ef_search);

    free(results);
    free(meta_buf);
    free(qvec);
}

/* ── main ───────────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <input.bin.gz> <db_path> [--limit N] [--ef-search N]\n"
        "\n"
        "  input.bin.gz   GoogleNews-vectors-negative300.bin.gz\n"
        "  db_path        Directory for the nn20db HNSW graph (created if new)\n"
        "  --limit N      Only ingest first N words (default: all)\n"
        "  --ef-search N  ef_search for self-test (default: %d)\n",
        prog, DEFAULT_EF_SEARCH);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char *gz_path = argv[1];
    const char *db_path = argv[2];
    long limit = 0;
    int ef_search = DEFAULT_EF_SEARCH;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atol(argv[++i]);
        } else if (strcmp(argv[i], "--ef-search") == 0 && i + 1 < argc) {
            ef_search = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    nn20db_config cfg = make_config(db_path, ef_search);
    int created = 0;

    printf("Opening/creating nn20db at '%s' ...\n", db_path);
    double t0 = now_seconds();
    NN20DB *db = open_or_create_db(&cfg, &created);
    if (!db) return 1;

    if (created) {
        printf("New database created. Ingesting vectors from '%s' ...\n", gz_path);
        long inserted = 0;
        if (ingest_word2vec(db, gz_path, limit, &inserted) != 0) {
            fprintf(stderr, "Ingest failed\n");
            nn20db_dtor(db);
            return 1;
        }
        printf("Ingested %ld vectors in %.1f s\n",
               inserted, now_seconds() - t0);
    } else {
        printf("Opened existing database in %.3f s\n", now_seconds() - t0);
    }

    run_self_test(db, ef_search);

    nn20db_dtor(db);
    printf("Done.\n");
    return 0;
}
