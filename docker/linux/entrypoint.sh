#!/bin/sh
set -eu

cd /linux

make allmodconfig
make -j"$(nproc)"
make compile_commands.json

exec askl-clang-indexer /linux \
  --project linux \
  -j "$(nproc)" \
  --progress \
  --include-git-files \
  --modules kbuild \
  -o /out \
  "$@"
