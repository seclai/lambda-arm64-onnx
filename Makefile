.PHONY: all image

ifndef CONTAINER_CLI
CONTAINER_CLI := docker
endif

all: image

image:
	$(CONTAINER_CLI) build -f Containerfile --tag lambda-arm64-onnx-shim:latest
