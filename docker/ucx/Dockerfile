# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=askl-clang-indexer:latest

FROM ${BASE_IMAGE}

USER root

# Enable non-free for CUDA packages
RUN sed -i 's/Components: main/Components: main contrib non-free non-free-firmware/' \
    /etc/apt/sources.list.d/debian.sources

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    make gcc g++ automake autoconf libtool pkg-config \
    bear git ca-certificates \
    libibverbs-dev librdmacm-dev \
    libnuma-dev \
    libfuse3-dev \
    libbfd-dev libiberty-dev \
    valgrind \
    nvidia-cuda-dev \
    libamdhip64-dev hipcc \
    && rm -rf /var/lib/apt/lists/*

# gdrcopy - no Debian package, build lib + headers from source
RUN git clone --depth 1 --branch v2.5.2 https://github.com/NVIDIA/gdrcopy.git /tmp/gdrcopy && \
    cd /tmp/gdrcopy && \
    make CUDA=/usr prefix=/usr/local lib lib_install && \
    ldconfig && \
    rm -rf /tmp/gdrcopy

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /ucx

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
