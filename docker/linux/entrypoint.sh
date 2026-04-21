#!/bin/sh
set -eu

if [ "$(id -u)" = "0" ]; then
    TARGET=$(stat -c %u:%g /linux)
    chown "$TARGET" /out
    exec gosu "$TARGET" "$0" "$@"
fi

cd /linux

make mrproper
make allmodconfig
make -j"$(nproc)"
make compile_commands.json

exec askl-clang-indexer /linux \
  --project linux \
  -j "$(nproc)" \
  --progress \
  --modules kbuild \
  -o /out \
  "$@"
