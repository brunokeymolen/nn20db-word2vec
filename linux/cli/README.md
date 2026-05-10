# linux/cli — Word2Vec interactive CLI

Interactive nearest-neighbour search using the nn20db HNSW index built by
`linux/build_index/`. Supports plain word queries, vector arithmetic
expressions, optional local search (via Python nn20db API) and remote search
on an ESP32-S3 (via TCP).

## Prerequisites

- Python 3.9+
- numpy: `pip install numpy`
- nn20db Python API (from the nn20db-sdk repo at `api/python/nn20db.py`)
- Optional: `gensim` (not required — the CLI parses `.bin.gz` directly)

## Usage

```bash
python word2vec_cli.py \
    --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz \
    [--limit N] \
    [--esp32 HOST:PORT] \
  [--local-search --db ~/data/word2vec_db] \
    [--k 10] \
  [--ef 15]
```

## Options

| Flag             | Default | Description                                             |
|------------------|---------|---------------------------------------------------------|
| `--db`           | —       | Path to the nn20db database directory (`--local-search`) |
| `--word-vectors` | —       | Path to `GoogleNews-vectors-negative300.bin.gz` (req.)  |
| `--limit N`      | all     | Load only the first N word vectors into RAM             |
| `--esp32 H:P`    | —       | Also search on ESP32-S3 TCP server at HOST:PORT         |
| `--local-search` | off     | Enable local Linux nn20db search                        |
| `--k N`          | 10      | Number of nearest neighbours to return                  |
| `--ef N`         | 15      | ef_search sent with each query unless overridden        |

## Session example

```
Loading word vectors from /data/GoogleNews-vectors-negative300.bin.gz ...
  File: 3000000 words, dim=300. Loading 200000.
  50000/200000  (12345 words/s)
  ...
Loaded 200000 words in 16.2 s
Opening nn20db at '/data/word2vec_db' ...
  nn20db opened OK

Word2Vec REPL  (type 'quit' to exit)
  Examples:  king
             king - man + woman

word2vec> king
  Query vector built from: king
  ESP32 Search...
  ESP32 search  (42.3 ms)
  [ESP32 192.168.1.42:9900]
    1  king                              0.000000
    2  kings                             0.285412
    3  queen                             0.391836
  ...

word2vec> king - man + woman
  Query vector built from: king, -man, woman
  ESP32 Search...
  ESP32 search  (42.3 ms)
  [ESP32 192.168.1.42:9900]
    1  queen                             0.310421
   ...

word2vec> king - man + woman ; 25
  Query vector built from: king, -man, woman
  ef_search override: 25
  ESP32 Search...
  ...

word2vec> quit
```

Enable local Linux search explicitly if you want it as well:

```bash
python word2vec_cli.py \
    --local-search \
    --db ~/data/word2vec_db \
    --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz \
    --esp32 192.168.1.42:9900
```

Then each query can run both local and remote search.

```text
word2vec> king - man + woman
  Query vector built from: king, -man, woman
  Local search  (9.1 ms)
  [Linux nn20db]
    1  queen                             0.310421
    2  princess                          0.381203
  ...
  ESP32 Search...
  ESP32 search  (42.3 ms)
  [ESP32 192.168.1.42:9900]
    1  queen                             0.310421
  ...
```

Add `; N` after an expression to override `ef_search` for that query only. If
the suffix is omitted, the CLI falls back to `--ef`.

## Wire protocol (ESP32 mode)

```
Linux → ESP32:  uint16 k + uint16 ef_search + 300 × float32 little-endian
               =  1204 bytes
ESP32 → Linux:  k  × (32-byte word + 4-byte float32)  =  k × 36 bytes
```
