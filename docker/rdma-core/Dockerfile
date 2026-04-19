# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=askl-clang-indexer:latest

FROM ${BASE_IMAGE}

USER root

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build gcc g++ \
    python3 \
    pkg-config \
    libnl-3-dev libnl-route-3-dev \
    libsystemd-dev libudev-dev \
    valgrind \
    libdrm-dev \
    cython3 python3-dev \
    pandoc python3-docutils \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /rdma-core

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
