#!/bin/sh
set -eu

cd /ucx

./autogen.sh

./contrib/configure-release \
  --enable-cma \
  --enable-mt \
  --enable-experimental-api \
  --enable-devel-headers \
  --enable-examples \
  --enable-test-apps \
  --with-valgrind \
  --with-verbs \
  --with-rdmacm \
  --with-rc \
  --with-ud \
  --with-dc \
  --with-mlx5 \
  --with-ib-hw-tm \
  --with-dm \
  --with-devx \
  --with-fuse3 \
  --with-bfd \
  --with-efa \
  --with-cuda=/usr \
  --with-rocm=/usr \
  --with-gdrcopy=/usr/local

bear -- make -j"$(nproc)"

exec askl-clang-indexer /ucx \
  --project ucx \
  -j "$(nproc)" \
  --progress \
  --include-git-files \
  -o /out \
  "$@"
