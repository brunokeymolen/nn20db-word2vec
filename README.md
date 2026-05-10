# nn20db-embedded-word2vect

Word2Vec nearest-neighbour search demo using [nn20db](https://github.com/brunokeymolen/nn20db-sdk)
HNSW on Linux + ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-1.47).

## Demo Video

[![Watch the demo on YouTube](https://img.youtube.com/vi/uMlM1yzEbDw/hqdefault.jpg)](https://www.youtube.com/watch?v=uMlM1yzEbDw)

## Overview

```
GoogleNews-vectors-negative300.bin.gz
        │
        ▼  linux/build_index/
  nn20db HNSW graph  ──── copy to SD card ────▶  ESP32-S3
        │                                          │
        ▼  linux/cli/                              │ TCP (port 9900)
  word2vec_cli.py  ◀─────── remote search ─────────┘
  (local search or proxy)
```

**Phase 1 design principle:** all word-to-vector lookup and vector arithmetic is
done on Linux. Linux sends a single 300-dimensional float32 query vector to
the ESP32-S3 over the network. The ESP32-S3 is search-only in phase 1.

## Hardware

| Item         | Details                                          |
|--------------|--------------------------------------------------|
| Board        | Waveshare ESP32-S3-Touch-LCD-1.47                |
| Chip         | ESP32-S3R8 (8 MB PSRAM, 16 MB Flash)             |
| Display      | 1.47″ IPS 172×320, JD9853 SPI, AXS5106L touch   |
| Storage      | TF/SD card slot (use 8–32 GB card)               |
| Connectivity | 2.4 GHz Wi-Fi 802.11 b/g/n                       |

## Repository Layout

```
nn20db-embedded-word2vect/
├── linux/
│   ├── build_index/   C tool: parse GoogleNews .bin.gz, build nn20db graph
│   └── cli/           Python REPL: word/expression queries, local + ESP32 search
├── esp32/
│   └── word2vec_search/  ESP32-S3 IDF app: SD search + TCP server + LVGL UI
└── sdk/               Populated by install-sdk.sh (gitignored)
```

## Prerequisites

- Linux x86-64 with GCC, cmake, make
- zlib development headers (`apt install zlib1g-dev`)
- Python 3.9+ with `numpy` and optionally `gensim`
- ESP-IDF v5.x (for ESP32 build)
- nn20db SDK release 1.1.0

## 1. Install the nn20db SDK

From the root of the nn20db-sdk repository (or run `install-sdk.sh` from here
by adjusting paths):

```bash
# Linux SDK
./scripts/install-sdk.sh linux \
  https://github.com/brunokeymolen/nn20db-sdk/releases/download/release-1.1.0/nn20db-sdk-linux-1.1.0.tar.gz

# ESP32-S3 SDK
./scripts/install-sdk.sh esp32 \
  https://github.com/brunokeymolen/nn20db-sdk/releases/download/release-1.1.0/nn20db-sdk-esp32s3-1.1.0.tar.gz
```

Both commands install into `sdk/linux/current/` and `sdk/esp32/current/`
relative to the nn20db-sdk repo root (not this project). Adjust
`SDK_ROOT` variables in the Makefiles if you install elsewhere.

## 2. Get the Word2Vec data

Download the GoogleNews vectors (publicly available, ~1.5 GB compressed):

```bash
# Huggingface:
wget https://huggingface.co/NathaNn1111/word2vec-google-news-negative-300-bin/resolve/main/GoogleNews-vectors-negative300.bin?download=true
# or:
# https://drive.google.com/file/d/0B7XkCwpI5KDYNlNUTTlSS21pQmM/
```

## 3. Build the index (Linux)

```bash
cd linux/build_index
make
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db
```

Optional: build a smaller index for development (`--limit 200000`):

```bash
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db_200k \
    --limit 200000
```

## 4. Run the CLI (Linux)

```bash
cd linux/cli
python word2vec_cli.py \
    --db ~/data/word2vec_db \
    --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz \
    --limit 200000
```

Interactive session:

```
word2vec> king
  1  king         0.0000
  2  kings        0.3012
  3  queen        0.3918
  ...

word2vec> king - man + woman
  1  queen        0.3104
  2  princess     0.3812
  ...

word2vec> quit
```

With ESP32 remote search:

```bash
python word2vec_cli.py \
    --db ~/data/word2vec_db \
    --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz \
    --esp32 192.168.1.42:9900
```

## 5. Copy the database to the SD card

```bash
# Mount your SD card, then:
cp -r ~/data/word2vec_db/ /media/$USER/<sdcard>/nand0/word2vec/
```

The ESP32 firmware expects the database at `/sdcard/nand0/word2vec/` on the
mounted FAT volume. Path components must be ≤ 8.3 characters.

## 6. Build and flash ESP32-S3

### ESP-IDF setup

Install the Espressif SDK locally, or use the development container included in this project.

The recommended and easiest setup is to use Visual Studio Code with Dev Containers:

1. Connect the ESP device to your computer first, so the serial port is available inside the container.
2. Open the project in Visual Studio Code.
3. Press `F1`.
4. Select `Dev Containers: Reopen in Container`.

After the container has started, the ESP-IDF environment should be available and the connected device should be visible from inside the container.

```bash
root@5d05d157e0bc:/workspaces/nn20db-embedded-word2vect/esp32/word2vec_search# get_idf 
Checking "python3" ...
Python 3.12.3
"python3" has been detected
Activating ESP-IDF 5.5
Setting IDF_PATH to '/opt/esp/idf'.
* Checking python version ... 3.12.3
* Checking python dependencies ... OK
* Deactivating the current ESP-IDF environment (if any) ... OK
* Establishing a new ESP-IDF environment ... OK
* Identifying shell ... bash
* Detecting outdated tools in system ... OK - no outdated tools found
* Shell completion ... Autocompletion code generated

Done! You can now compile ESP-IDF projects.
```

Set your Wi-Fi credentials first, create the file `esp32/word2vec_search/sdkconfig.defaults.local`


```bash
root@5d05d157e0bc:/workspaces/nn20db-embedded-word2vect# cat esp32/word2vec_search/sdkconfig.defaults.local
```
```conf
# Local Wi-Fi credentials — NOT committed to git (see .gitignore)
CONFIG_WORD2VEC_WIFI_SSID="<your access point ssid>"
CONFIG_WORD2VEC_WIFI_PASSWORD="<your password>"
```

#### Build, Flash and Monitor
```bash
cd esp32/word2vec_search
idf.py set-target esp32s3
make build
make flash-monitor
```



## Vector encoding

All vectors are L2-normalised to unit length before insertion and before
searching. This makes Euclidean distance a monotone proxy for cosine
similarity: `||a - b||² = 2 - 2·cos(θ)`.

Both Linux and ESP32 use `METRIC_EUCLIDEAN_F32_CONFIG`.

## Wire protocol

```
Linux → ESP32:   uint16 k + uint16 ef_search + 300 × float32 little-endian  (1204 bytes)
ESP32 → Linux:   k × (32-byte word-label + 4-byte float32 distance)  (k×36 bytes)
```

In the CLI REPL, append `; N` to override `ef_search` for one query, for
example `king + woman ; 25`. Without the suffix, the CLI default stays in use.

## HNSW parameters

| Parameter          | Value | Notes                                    |
|--------------------|-------|------------------------------------------|
| M (level 0)        | 32    | Dense graph; good for 300-D vectors      |
| ef_construction    | 400   | High-quality build                       |
| ef_search (Linux)  | 100   | Default CLI; tune with `--ef`            |
| ef_search (ESP32)  | 32    | Faster on device; still good recall      |
