# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=askl-clang-indexer:latest

FROM ${BASE_IMAGE}

USER root

RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc flex bison bc \
    libssl-dev libelf-dev openssl xz-utils gawk \
    python3 perl git \
    dwarves cpio rsync \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENV HOME=/tmp
WORKDIR /linux

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
