# esp32/word2vec_search — ESP32-S3 Word2Vec search server

Runs nn20db HNSW search on a pre-built Word2Vec graph stored on SD card.
Exposes a simple TCP API so the Linux CLI can submit a 300-D float32 query
vector and receive top-10 results. Displays results on the onboard
172×320 IPS display using LVGL.

## Hardware

Waveshare ESP32-S3-Touch-LCD-1.47:
- ESP32-S3R8 (8 MB Octal PSRAM, 16 MB Flash)
- 1.47" IPS 172×320, JD9853 (SPI), AXS5106L touch (I2C)
- TF card slot

## Prerequisites

- ESP-IDF v5.x installed and sourced (`idf.py` on `$PATH`)
- nn20db ESP32-S3 SDK installed (see top-level README)
- The nn20db Word2Vec database built on Linux and copied to SD card
- Internet access on first configure/build so ESP-IDF can fetch managed
    components (`espressif/esp_lvgl_port` and `lvgl/lvgl`)

This project uses ESP-IDF managed components. In `main/CMakeLists.txt`, the
component names must match the managed names exposed to CMake:
`lvgl__lvgl` and `espressif__esp_lvgl_port`.

## Build the database (Linux first!)

```bash
cd ../../linux/build_index
make
./build_word2vec_index \
    ~/data/GoogleNews-vectors-negative300.bin.gz \
    ~/data/word2vec_db \
    --limit 200000   # start with 200k for development
```

Copy to SD card:

```bash
cp -r ~/data/word2vec_db/ /media/$USER/<sdcard>/nand0/word2vec/
```

## Configure Wi-Fi credentials

```bash
idf.py menuconfig
# Navigate: Word2Vec Search Configuration → Wi-Fi SSID / Password
```

## Build for ESP32-S3 (first time)

```bash
rm -rf build sdkconfig
idf.py set-target esp32s3
make build
```

### Product quantization (PQ)

PQ-compressed vectors are used by default (expects the database at
`/sdcard/nand0/w2vpq`, matching `--limit`/`config_pq.h`). To build for the
full fp32 database instead (`/sdcard/nand0/w2vfp32`, `config_fp32.h`):

```bash
# with PQ (default)
make build
# or in the container:
make docker-build

# without PQ
make build USE_PQ=0
# or in the container:
make docker-build USE_PQ=0
```

## Flash and monitor

```bash
make flash-monitor
# or with explicit port:
make flash-monitor PORT=/dev/ttyUSB0
```

## Expected boot output

```
I (...)  main: Mounting SD card ...
I (...)  main: SD mounted OK (SC16G, 15193 MB)
I (...)  main: Connecting to Wi-Fi SSID: myssid
I (...)  main: Wi-Fi connected, IP: 192.168.1.42
I (...)  main: Opening nn20db at /sdcard/nand0/word2vec ...
I (...)  main: nn20db opened OK
I (...)  main: TCP server started on port 9900
I (...)  main: Ready. Waiting for queries on 192.168.1.42:9900
```

The display shows:

```
┌─────────────────────┐
│   Word2Vec Search   │
│─────────────────────│
│  Wi-Fi: 192.168.x.y │
│                     │
│  Waiting for query  │
└─────────────────────┘
```

After a query arrives from the Linux CLI, the display transitions to a
spinner then shows the top-10 results list.

## Wire protocol

```
Linux → ESP32:   300 × float32 LE  =  1200 bytes
ESP32 → Linux:   10 × (32-byte word + 4-byte float32)  =  360 bytes
```

## Pin assignments

| Signal    | GPIO | Notes                                   |
|-----------|------|-----------------------------------------|
| LCD SCK   | 38   | SPI clock                               |
| LCD MOSI  | 39   | SPI data                                |
| LCD CS    | 21   | SPI chip select                         |
| LCD DC    | 45   | Data/command                            |
| LCD RST   | 40   | Hardware reset                          |
| LCD BL    | 46   | Backlight PWM                           |
| Touch SDA | 42   | I2C data (AXS5106L)                     |
| Touch SCL | 41   | I2C clock                               |
| Touch INT | 47   | Interrupt (optional)                    |
| Touch RST | 48   | Reset (optional)                        |
| SD CLK    | 16   | SDMMC native clock                      |
| SD CMD    | 15   | SDMMC command                           |
| SD D0     | 17   | SDMMC data 0                            |
| SD D1     | 18   | SDMMC data 1                            |
| SD D2     | 13   | SDMMC data 2                            |
| SD D3     | 14   | SDMMC data 3                            |

> Pin assignments verified against the Waveshare ESP32-S3-Touch-LCD-1.47
> official ESP-IDF factory demo BSP (`bsp_display.h`, `bsp_sdcard.h`).

## sdkconfig files

| File                        | Purpose                                        |
|-----------------------------|------------------------------------------------|
| `sdkconfig.defaults`        | Shared: FreeRTOS stacks, FAT, VFS, USB CDC     |
| `sdkconfig.defaults.esp32s3`| S3: Octal PSRAM, Wi-Fi, 16 MB Flash            |
