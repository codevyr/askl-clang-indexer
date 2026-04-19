#!/bin/sh
set -eu

cd /rdma-core

cmake -B build -GNinja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_RESOLVE_NEIGH=1 \
  -DENABLE_VALGRIND=1 \
  -DENABLE_IBDIAGS_COMPAT=True \
  -DIN_PLACE=1

ninja -C build -j"$(nproc)"

exec askl-clang-indexer /rdma-core/build \
  --project rdma-core \
  -j "$(nproc)" \
  --progress \
  --include-git-files \
  -o /out \
  "$@"
