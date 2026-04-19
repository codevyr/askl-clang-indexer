# syntax=docker/dockerfile:1.7

ARG LLVM_VERSION=19

FROM debian:trixie AS build

ARG LLVM_VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ git ca-certificates \
    libclang-${LLVM_VERSION}-dev \
    protobuf-compiler libprotobuf-dev libabsl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY . .

RUN cmake -B build \
    -DLIBCLANG_PATH=/usr/lib/llvm-${LLVM_VERSION}/lib \
    -DLIBCLANG_INCLUDE_PATH=/usr/lib/llvm-${LLVM_VERSION}/include \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)

FROM debian:trixie-slim

ARG LLVM_VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
    libclang1-${LLVM_VERSION} \
    libclang-common-${LLVM_VERSION}-dev \
    libprotobuf32t64 \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 65532 nonroot \
    && useradd --uid 65532 --gid 65532 --home-dir /home/nonroot --create-home --shell /usr/sbin/nologin nonroot \
    && mkdir -p /workspace /out \
    && chown -R 65532:65532 /workspace /out /home/nonroot

WORKDIR /workspace

COPY --from=build /src/build/askl-clang-indexer /usr/local/bin/askl-clang-indexer

USER 65532:65532

ENTRYPOINT ["/usr/local/bin/askl-clang-indexer"]
CMD ["--help"]
