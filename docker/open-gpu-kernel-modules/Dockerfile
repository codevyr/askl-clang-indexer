# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=askl-clang-indexer:latest

FROM ${BASE_IMAGE}

USER root

RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc g++ bear \
    linux-headers-amd64 \
    libelf-dev libssl-dev openssl kmod xz-utils \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENV HOME=/tmp
WORKDIR /open-gpu-kernel-modules

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
