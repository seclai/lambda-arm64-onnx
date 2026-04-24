FROM debian:bookworm-slim AS builder

RUN mkdir /shim

WORKDIR /shim

# Build a shared library that will shim the cpuinfo file

COPY cpuinfo_shim.c /shim/cpuinfo_shim.c

RUN apt-get update \
    && apt-get install -y gcc curl \
    && gcc -shared -fPIC -o /shim/cpuinfo_shim.so /shim/cpuinfo_shim.c -ldl

# You can replace this with any other base image that you want to use for your final container
FROM python:3.13-slim

# Install the shared library from the builder stage
COPY --from=builder /shim/cpuinfo_shim.so /usr/lib/cpuinfo_shim.so

# Preload the shared library before the ONNX Runtime is loaded
ENV LD_PRELOAD=/usr/lib/cpuinfo_shim.so

RUN pip install onnxruntime
