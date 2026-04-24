# lambda-arm64-onnx

An `LD_PRELOAD` shim that fixes ONNX Runtime crashing on AWS Lambda ARM64 (Graviton) instances.

## The Problem

ONNX Runtime bundles the [pytorch/cpuinfo](https://github.com/pytorch/cpuinfo) library, which reads CPU topology from two sysfs files:

- `/sys/devices/system/cpu/possible`
- `/sys/devices/system/cpu/present`

On AWS Lambda ARM64 (Graviton 2/3) these files **do not exist**. This causes cpuinfo to crash with an unhandled C++ exception before ORT's logging manager is even initialized:

```
Attempt to use DefaultLogger but none has been registered.
```

This has been a known, unfixed issue since December 2021:

- [onnxruntime#10038](https://github.com/microsoft/onnxruntime/issues/10038)
- [onnxruntime#15650](https://github.com/microsoft/onnxruntime/issues/15650)

## How It Works

The shim ([cpuinfo_shim.c](cpuinfo_shim.c)) is a small shared library loaded via `LD_PRELOAD` that intercepts `fopen()` and `open()`. When the real sysfs file is missing **and** the requested path is one of the two CPU topology files, the shim returns a synthetic file containing `0-N\n` (where N is the online CPU count minus one, obtained from `sysconf(_SC_NPROCESSORS_ONLN)`). For all other paths the real syscall result is returned unchanged.

- `fopen()` — returns a `FILE*` backed by `fmemopen()`
- `open()` — returns a file descriptor backed by `memfd_create()`

## Repository Contents

| File | Description |
|---|---|
| [cpuinfo_shim.c](cpuinfo_shim.c) | The `LD_PRELOAD` shim source code |
| [Containerfile](Containerfile) | Multi-stage container build (compile shim → install into final image) |
| [Makefile](Makefile) | Convenience targets for building the container image |
| [LICENSE](LICENSE) | MIT License |

## Building the Container Image

### Using Make (recommended)

```bash
# Uses Docker by default
make

# Use Podman or any other CLI
make CONTAINER_CLI=podman
```

### Using a container CLI directly

```bash
# Docker
docker build -f Containerfile --tag lambda-arm64-onnx-shim:latest .

# Podman
podman build -f Containerfile --tag lambda-arm64-onnx-shim:latest .

# nerdctl, etc.
nerdctl build -f Containerfile --tag lambda-arm64-onnx-shim:latest .
```

## Using the Image

The provided [Containerfile](Containerfile) is a **starting point** — it uses `python:3.13-slim` as the final stage and installs `onnxruntime` via pip. Adapt it to your own base image and application code:

```dockerfile
FROM debian:bookworm-slim AS builder

RUN mkdir /shim
WORKDIR /shim
COPY cpuinfo_shim.c /shim/cpuinfo_shim.c
RUN apt-get update \
    && apt-get install -y gcc \
    && gcc -shared -fPIC -o /shim/cpuinfo_shim.so /shim/cpuinfo_shim.c -ldl

# Replace with your own base image
FROM public.ecr.aws/lambda/python:3.13

COPY --from=builder /shim/cpuinfo_shim.so /usr/lib/cpuinfo_shim.so
ENV LD_PRELOAD=/usr/lib/cpuinfo_shim.so

# Install your dependencies and copy your application code
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY app/ ./app/
```

The two key lines are:

```dockerfile
COPY --from=builder /shim/cpuinfo_shim.so /usr/lib/cpuinfo_shim.so
ENV LD_PRELOAD=/usr/lib/cpuinfo_shim.so
```

## Building the Shared Library Manually

If you prefer to compile the `.so` outside of a container (e.g. on an ARM64 EC2 instance or in CI):

```bash
gcc -shared -fPIC -o cpuinfo_shim.so cpuinfo_shim.c -ldl
```

Then copy `cpuinfo_shim.so` into your deployment package and set the environment variable:

```bash
export LD_PRELOAD=/path/to/cpuinfo_shim.so
```

> **Note:** The library must be compiled for the same architecture (ARM64/aarch64) as your Lambda runtime. If building on x86_64, use a cross-compiler or build inside an ARM64 container.

## Performance

The shim has **zero overhead** on normal operation. It only activates when:

1. The path matches one of the two CPU topology sysfs files, **and**
2. The real `fopen()`/`open()` call already failed (file does not exist)

On environments where the sysfs files exist (e.g. EC2, ECS), the real call succeeds and the shim is never invoked. The `LD_PRELOAD` can safely be left in place across all environments.

## CI/CD Example

A GitHub Actions workflow that builds and pushes a multi-arch image (ARM64 + x86_64):

```yaml
name: Build and push container image

on:
  push:
    branches: [main]

jobs:
  build:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      id-token: write
    steps:
      - uses: actions/checkout@v4

      - uses: docker/setup-qemu-action@v3

      - uses: docker/setup-buildx-action@v3

      # Replace with your own registry login step
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - uses: docker/build-push-action@v6
        with:
          context: .
          file: Containerfile
          platforms: linux/arm64,linux/amd64
          push: true
          tags: ghcr.io/${{ github.repository }}:latest
```

> **Tip:** The shim only activates on ARM64 when the sysfs files are missing. On x86_64 the topology files normally exist, so the shim is a harmless no-op — a single multi-arch image works everywhere.

## Troubleshooting

### Verify the shim is loaded

```bash
# Inside the container
echo $LD_PRELOAD
# Should print: /usr/lib/cpuinfo_shim.so

# Check it's picked up by the dynamic linker
LD_DEBUG=libs python -c "import onnxruntime" 2>&1 | grep cpuinfo_shim
```

### "cannot open shared object file"

The shim `.so` is missing or at a different path than `LD_PRELOAD` expects:

```bash
ls -la /usr/lib/cpuinfo_shim.so
```

Make sure the `COPY --from=builder` destination matches the `LD_PRELOAD` path.

### Still crashing with the DefaultLogger error

1. Confirm you're running on ARM64:
   ```bash
   uname -m   # should print aarch64
   ```
2. Confirm the sysfs files are actually missing (this is the condition the shim fixes):
   ```bash
   cat /sys/devices/system/cpu/possible   # expect "No such file or directory"
   ```
3. Confirm the shim is compiled for the right architecture:
   ```bash
   file /usr/lib/cpuinfo_shim.so   # should say "ELF 64-bit LSB shared object, ARM aarch64"
   ```

### Works locally but fails on Lambda

Local Docker on macOS/Linux typically provides the sysfs files via the VM. Lambda does not. This is the expected scenario — the shim is only needed in environments where the files are absent.

## Compatibility

- **Architectures:** ARM64 / aarch64 (Graviton 2, Graviton 3, Graviton 4)
- **ONNX Runtime:** All versions affected by the cpuinfo crash (1.x and 2.x as of writing)
- **OS:** Any Linux distribution with glibc (the shim uses `memfd_create`, `fmemopen`, and `dlsym`)
- **Runtime:** Works with AWS Lambda, ECS, EKS, EC2, or any other ARM64 Linux environment where the sysfs files are absent

## License

[MIT](LICENSE) — Copyright (c) 2026 Seclai, Inc.

