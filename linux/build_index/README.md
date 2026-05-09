# linux/build_index — Word2Vec index builder

Reads `GoogleNews-vectors-negative300.bin.gz`, normalises every vector to unit
length, and builds a persistent nn20db HNSW graph on disk.

On the first run the database is **created** and all vectors are inserted.
On subsequent runs the existing database is **opened** and a recall self-test
is executed.

## Prerequisites

- nn20db Linux SDK installed at `sdk/linux/current/` (see top-level README)
- zlib development headers: `sudo apt install zlib1g-dev`

## Build

```bash
make
```

Or with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

Full vocabulary (~3M words, needs ~4 GB RAM and a large SD card):

```bash
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db
```

Reduced vocabulary for development (200k words, ~250 MB):

```bash
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db_200k \
    --limit 200000
```

Higher ef_search for better recall measurement:

```bash
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db \
    --ef-search 200
```

More self-test vectors for steadier timing:

```bash
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db \
    --test 10000
```

## Self-test output (example)

```
Word2Vec file: 3000000 words, 300 dimensions
  done: inserted 3000000 vectors in 3842.1 s

── Self-test: recall@10 from 'GoogleNews-vectors-negative300.bin' ──
  collecting 10000 test vectors ...
  done in 5000 ms (10000 vectors, source=3000000)
  hnsw searches of 10000 vectors ...
  done in 2000 ms
  recall@10 = 0.9850 (9850/10000)  ef_search=100
  QPS = 5000.0  avg_search=0.200 ms  successful_searches=10000/10000
Done.
```

## Copy to SD card

```bash
cp -r ~/data/word2vec_db/ /media/$USER/<sdcard>/nand0/word2vec/
```

The ESP32 firmware expects the database at `/sdcard/nand0/word2vec/`.
