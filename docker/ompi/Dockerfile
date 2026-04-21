# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=askl-clang-indexer:latest

FROM ${BASE_IMAGE}

USER root

# Enable non-free for CUDA packages
RUN sed -i 's/Components: main/Components: main contrib non-free non-free-firmware/' \
    /etc/apt/sources.list.d/debian.sources

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    make gcc g++ gfortran automake autoconf libtool pkg-config flex \
    bear git ca-certificates perl python3 \
    libnl-3-dev libnl-route-3-dev \
    libevent-dev libhwloc-dev \
    \
    libibverbs-dev librdmacm-dev \
    libfabric-dev \
    libucx-dev \
    libnuma-dev \
    valgrind \
    zlib1g-dev \
    nvidia-cuda-dev \
    libamdhip64-dev hipcc \
    && rm -rf /var/lib/apt/lists/*

# OMPI checks /usr/lib for libamdhip64 but Debian puts it in /usr/lib/x86_64-linux-gnu
RUN ln -s /usr/lib/x86_64-linux-gnu/libamdhip64.so /usr/lib/libamdhip64.so

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENV HOME=/tmp
WORKDIR /ompi

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
