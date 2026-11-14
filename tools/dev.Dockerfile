# Build/test image for the engine. Same toolchain as CI (ubuntu-24.04).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-14 clang-18 lld-18 llvm-18 libclang-rt-18-dev cmake ninja-build \
      libgtest-dev libssl-dev liburing-dev pkg-config ca-certificates python3 git curl \
      linux-tools-common iproute2 \
 && rm -rf /var/lib/apt/lists/* \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 \
 && update-alternatives --install /usr/bin/llvm-profdata llvm-profdata /usr/bin/llvm-profdata-18 100
WORKDIR /src
