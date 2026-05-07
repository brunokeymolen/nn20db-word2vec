PORT        ?= /dev/ttyACM0
PROJECT_DIR := $(shell pwd)
REPO_ROOT   := $(abspath $(PROJECT_DIR)/../..)
IMAGE_NAME  := nn20db-esp32-idf
DOCKERFILE  := $(REPO_ROOT)/.devcontainer/esp32/Dockerfile

# Docker run without TTY (build/clean - no USB needed)
DOCKER_RUN := docker run --rm \
	-v $(REPO_ROOT):/workspace \
	-w /workspace/esp32/word2vec_search \
	$(IMAGE_NAME)

# Docker run with TTY + USB passthrough (flash/monitor)
DOCKER_RUN_TTY := docker run --rm -it \
	--privileged \
	--volume=/dev/bus/usb:/dev/bus/usb \
	--device-cgroup-rule="c 166:* rmw" \
	--device-cgroup-rule="c 188:* rmw" \
	-v $(REPO_ROOT):/workspace \
	-w /workspace/esp32/word2vec_search \
	$(IMAGE_NAME)

.PHONY: all build flash monitor flash-monitor set-target-s3 clean \
        docker-image docker-build docker-flash docker-monitor \
        docker-flash-monitor docker-set-target docker-clean

# -- native idf.py targets (requires ESP-IDF sourced on host) -----------------
all: build

build:
	idf.py build

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor --no-reset

flash-monitor:
	idf.py -p $(PORT) flash monitor --no-reset

set-target-s3:
	rm -rf build sdkconfig
	idf.py set-target esp32s3

clean:
	idf.py fullclean

# -- Docker targets (no host IDF required) ------------------------------------
docker-image:
	docker build -t $(IMAGE_NAME) -f $(DOCKERFILE) $(REPO_ROOT)/.devcontainer/esp32

docker-set-target: docker-image
	$(DOCKER_RUN) sh -c "rm -rf build sdkconfig && idf.py set-target esp32s3"

docker-build: docker-image
	$(DOCKER_RUN) sh -c "[ -f sdkconfig ] || idf.py set-target esp32s3; idf.py build"

docker-flash: docker-image
	$(DOCKER_RUN_TTY) idf.py -p $(PORT) flash

docker-monitor: docker-image
	$(DOCKER_RUN_TTY) idf.py -p $(PORT) monitor --no-reset

docker-flash-monitor: docker-image
	$(DOCKER_RUN_TTY) idf.py -p $(PORT) flash monitor --no-reset

docker-clean: docker-image
	$(DOCKER_RUN) idf.py fullclean
