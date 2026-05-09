/* Enable POSIX clock_gettime / CLOCK_MONOTONIC */
#define _POSIX_C_SOURCE 200809L

/*
 * nn20db-embedded-word2vect
 *
 * build_word2vec_index — Load GoogleNews Word2Vec vectors, normalize to unit
 * length, and build a persistent nn20db HNSW index.
 *
 * Usage:
 *   ./build_word2vec_index <input.bin> <db_path> [--limit N] [--ef-search N] [--test N]
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

#include "nn20db.h"
#include "nn20db_config.h"

/* ── constants ─────────────────────────────────────────────────────────────── */

#define WORD2VEC_DIM        300
#define METADATA_SIZE       32      /* sizeof(word_meta_t) */
#define DEFAULT_EF_SEARCH   100
#define DEFAULT_SELF_TEST_QUERIES 10000   /* queries used for recall self-test */
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

static void set_word_meta(word_meta_t *meta, const char *word) {
    memset(meta, 0, sizeof(*meta));
    size_t n = strnlen(word, sizeof(meta->word) - 1);
    memcpy(meta->word, word, n);
}

/* ── nn20db config ──────────────────────────────────────────────────────────── */

static nn20db_config make_config(const char *db_path, int ef_search) {
    nn20db_config cfg = {
        .vector = {
            .type          = NN20DB_DIMENSION_FLOAT32_CONFIG,
            .dimension     = WORD2VEC_DIM,
            .metadata_size = METADATA_SIZE,
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
            .type = METRIC_EUCLIDEAN_F32_CONFIG,
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
                           const char *bin_path,
                           long limit,
                           long *inserted_out)
{
    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", bin_path);
        return -1;
    }

    /* read header */
    char header[256];
    if (!fgets(header, sizeof(header), f)) {
        fprintf(stderr, "Failed to read header\n");
        fclose(f);
        return -1;
    }
    long total_words, dim;
    if (sscanf(header, "%ld %ld", &total_words, &dim) != 2) {
        fprintf(stderr, "Bad header: '%s'\n", header);
        fclose(f);
        return -1;
    }
    if (dim != WORD2VEC_DIM) {
        fprintf(stderr, "Unexpected dimension %ld (expected %d)\n", dim, WORD2VEC_DIM);
        fclose(f);
        return -1;
    }

    printf("Word2Vec file: %ld words, %ld dimensions\n", total_words, dim);
    if (limit > 0 && limit < total_words) {
        printf("Limiting ingest to %ld words\n", limit);
        total_words = limit;
    }

    float *vec = malloc((size_t)dim * sizeof(float));
    if (!vec) {
        fclose(f);
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
        while ((c = fgetc(f)) != EOF && c != ' ' && c != '\n') {
            if (wlen < (int)(sizeof(wordbuf) - 1)) {
                wordbuf[wlen++] = (char)c;
            }
        }
        wordbuf[wlen] = '\0';

        if (wlen == 0) {
            /* skip blank / newline-only entries */
            if (fread(vec, sizeof(float), (size_t)dim, f) != (size_t)dim) {
                fprintf(stderr, "\nShort read at blank word %ld\n", wi);
                rc_overall = -1;
                break;
            }
            continue;
        }

        /* read raw floats */
        size_t nread = fread(vec, sizeof(float), (size_t)dim, f);
        if (nread != (size_t)dim) {
            fprintf(stderr, "\nShort read at word %ld (%s), got %zu floats\n",
                    wi, wordbuf, nread);
            rc_overall = -1;
            break;
        }

        normalize_l2(vec, (int)dim);

        word_meta_t meta;
        set_word_meta(&meta, wordbuf);

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

        if (inserted % 1000 == 0) {
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
    fclose(f);
    *inserted_out = inserted;
    return rc_overall;
}

/* ── self-test: sample query words from the source file, check recall ───────── */

typedef struct {
    word_meta_t meta;
    float vec[WORD2VEC_DIM];
} self_test_query_t;

static int read_word_label(FILE *f, char *wordbuf, size_t wordbuf_size) {
    int wlen = 0;
    int c;

    while ((c = fgetc(f)) != EOF && c != ' ' && c != '\n') {
        if (wlen < (int)(wordbuf_size - 1)) {
            wordbuf[wlen++] = (char)c;
        }
    }
    wordbuf[wlen] = '\0';

    return c == EOF ? -1 : wlen;
}

static int read_word2vec_record(FILE *f, float *vec, long dim, word_meta_t *meta) {
    char wordbuf[512];
    int wlen = read_word_label(f, wordbuf, sizeof(wordbuf));
    if (wlen < 0) {
        return -1;
    }

    if (fread(vec, sizeof(float), (size_t)dim, f) != (size_t)dim) {
        return -1;
    }

    if (meta != NULL) {
        set_word_meta(meta, wlen > 0 ? wordbuf : "");
    }

    return 0;
}

static int collect_self_test_queries(const char *bin_path,
                                     long n_inserted,
                                     int requested_queries,
                                     self_test_query_t **queries_out,
                                     int *query_count_out,
                                     long *source_words_out)
{
    *queries_out = NULL;
    *query_count_out = 0;
    *source_words_out = 0;

    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        fprintf(stderr, "Self-test: cannot open '%s'\n", bin_path);
        return -1;
    }

    char header[256];
    if (!fgets(header, sizeof(header), f)) {
        fprintf(stderr, "Self-test: failed to read header\n");
        fclose(f);
        return -1;
    }

    long total_words, dim;
    if (sscanf(header, "%ld %ld", &total_words, &dim) != 2 || dim != WORD2VEC_DIM) {
        fprintf(stderr, "Self-test: bad header\n");
        fclose(f);
        return -1;
    }

    if (n_inserted > 0 && n_inserted < total_words) {
        total_words = n_inserted;
    }
    *source_words_out = total_words;

    int query_count = requested_queries;
    if (query_count <= 0) {
        query_count = DEFAULT_SELF_TEST_QUERIES;
    }
    if ((long)query_count > total_words) {
        query_count = (int)total_words;
    }
    if (query_count <= 0) {
        fprintf(stderr, "Self-test: no source vectors available\n");
        fclose(f);
        return -1;
    }

    self_test_query_t *queries = calloc((size_t)query_count, sizeof(*queries));
    if (!queries) {
        fprintf(stderr, "Self-test: allocation failed\n");
        fclose(f);
        return -1;
    }

    long step = total_words / query_count;
    if (step < 1) {
        step = 1;
    }

    long wi = 0;
    int collected = 0;
    for (int qi = 0; qi < query_count; qi++) {
        long target = qi * step;
        if (target >= total_words) {
            target = total_words - 1;
        }

        while (wi < target) {
            float scratch[WORD2VEC_DIM];
            if (read_word2vec_record(f, scratch, dim, NULL) != 0) {
                goto done;
            }
            wi++;
        }

        if (read_word2vec_record(f, queries[collected].vec, dim,
                                 &queries[collected].meta) != 0) {
            goto done;
        }
        normalize_l2(queries[collected].vec, (int)dim);
        wi++;
        collected++;
    }

done:
    fclose(f);

    if (collected == 0) {
        free(queries);
        fprintf(stderr, "Self-test: failed to collect query vectors\n");
        return -1;
    }

    *queries_out = queries;
    *query_count_out = collected;
    return 0;
}

/*
 * Sample evenly-spaced words from the original .bin file,
 * search the DB for each, and check that the exact word appears in top-K.
 * This avoids any dependence on sequential or predictable nn20db IDs.
 */
static void run_self_test(NN20DB *db, int ef_search, const char *bin_path,
                          long n_inserted, int requested_queries) {
    printf("\n── Self-test: recall@%d from '%s' ──\n", SELF_TEST_K, bin_path);

    self_test_query_t *queries = NULL;
    int query_count = 0;
    long source_words = 0;

    printf("  collecting %d test vectors ...\n",
           requested_queries > 0 ? requested_queries : DEFAULT_SELF_TEST_QUERIES);
    double collect_t0 = now_seconds();
    if (collect_self_test_queries(bin_path, n_inserted, requested_queries,
                                  &queries, &query_count, &source_words) != 0) {
        return;
    }
    double collect_ms = (now_seconds() - collect_t0) * 1000.0;
    printf("  done in %.0f ms (%d vectors, source=%ld)\n",
           collect_ms, query_count, source_words);

    nn20db_vector_search_result *results =
        malloc((size_t)SELF_TEST_K * sizeof(*results));
    word_meta_t *meta_buf = malloc((size_t)SELF_TEST_K * sizeof(*meta_buf));

    if (!results || !meta_buf) {
        fprintf(stderr, "Self-test: allocation failed\n");
        free(results); free(meta_buf); free(queries);
        return;
    }

    int hits = 0;
    int searched = 0;
    double total_search_ms = 0.0;

    printf("  hnsw searches of %d vectors ...\n", query_count);
    for (int qi = 0; qi < query_count; qi++) {
        double t0 = now_seconds();
        int rc = nn20db_vector_search_ef(db, queries[qi].vec, SELF_TEST_K,
                                         ef_search, results);
        total_search_ms += (now_seconds() - t0) * 1000.0;

        if (rc != NN20DB_ERROR_OK) continue;
        searched++;

        for (int ri = 0; ri < SELF_TEST_K; ri++) {
            if (nn20db_vector_get(db, results[ri].id, NULL, &meta_buf[ri]) != NN20DB_ERROR_OK)
                memset(&meta_buf[ri], 0, sizeof(meta_buf[ri]));
        }

        for (int ri = 0; ri < SELF_TEST_K; ri++) {
            if (strncmp(queries[qi].meta.word, meta_buf[ri].word, 32) == 0) {
                hits++;
                break;
            }
        }
    }

    double qps = total_search_ms > 0.0
        ? ((double)query_count * 1000.0 / total_search_ms)
        : 0.0;
    double recall = query_count > 0 ? (double)hits / (double)query_count : 0.0;
    double avg_ms = query_count > 0 ? total_search_ms / (double)query_count : 0.0;

    printf("  done in %.0f ms\n", total_search_ms);
    printf("  recall@%d = %.4f (%d/%d)  ef_search=%d\n",
           SELF_TEST_K, recall, hits, query_count, ef_search);
    printf("  QPS = %.1f  avg_search=%.3f ms  successful_searches=%d/%d\n",
           qps, avg_ms, searched, query_count);

    free(results);
    free(meta_buf);
    free(queries);
}

/* ── main ───────────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <input.bin> <db_path> [--limit N] [--ef-search N] [--test N]\n"
        "\n"
        "  input.bin      GoogleNews-vectors-negative300.bin\n"
        "  db_path        Directory for the nn20db HNSW graph (created if new)\n"
        "  --limit N      Only ingest first N words (default: all)\n"
        "  --ef-search N  ef_search for self-test (default: %d)\n"
        "  --test N       Number of self-test vectors (default: %d)\n",
        prog, DEFAULT_EF_SEARCH, DEFAULT_SELF_TEST_QUERIES);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char *bin_path = argv[1];
    const char *db_path = argv[2];
    long limit = 0;  /* max vectors to ingest (for quick testing) */
    int ef_search = DEFAULT_EF_SEARCH;
    int self_test_queries = DEFAULT_SELF_TEST_QUERIES;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atol(argv[++i]);
        } else if (strcmp(argv[i], "--ef-search") == 0 && i + 1 < argc) {
            ef_search = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
            self_test_queries = atoi(argv[++i]);
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

    long inserted = 0;
    if (created) {
        printf("New database created. Ingesting vectors from '%s' ...\n", bin_path);
        if (ingest_word2vec(db, bin_path, limit, &inserted) != 0) {
            fprintf(stderr, "Ingest failed\n");
            nn20db_dtor(db);
            return 1;
        }
        printf("Ingested %ld vectors in %.1f s\n",
               inserted, now_seconds() - t0);
    } else {
        printf("Opened existing database in %.3f s\n", now_seconds() - t0);
        inserted = limit > 0 ? limit : 0;  /* 0 means use full source file */
    }

    run_self_test(db, ef_search, bin_path, inserted, self_test_queries);

    nn20db_dtor(db);
    printf("Done.\n");
    return 0;
}
