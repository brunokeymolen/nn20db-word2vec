CC      ?= cc
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra
TARGET   = build_word2vec_index
SRCS     = main.c

# SDK paths
# Override: make SDK_ROOT=/custom/path
REPO_ROOT  := $(abspath ../../..)
SDK_ROOT   ?= $(REPO_ROOT)/sdk/linux/current
PKG_CONFIG_PATH := $(SDK_ROOT)/linux/x86_64-linux-gnu/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH

NN20DB_CFLAGS := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --cflags nn20db)
NN20DB_LIBS   := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static nn20db)
ZLIB_CFLAGS   := $(shell pkg-config --cflags zlib 2>/dev/null || echo "")
ZLIB_LIBS     := $(shell pkg-config --libs   zlib 2>/dev/null || echo "-lz")

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(NN20DB_CFLAGS) $(ZLIB_CFLAGS) -o $@ $(SRCS) $(NN20DB_LIBS) $(ZLIB_LIBS) -lm

build-cmake:
	cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

clean:
	rm -f $(TARGET)
	rm -rf build/
