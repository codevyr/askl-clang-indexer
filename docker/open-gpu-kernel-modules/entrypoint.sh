#!/bin/sh
set -eu

if [ "$(id -u)" = "0" ]; then
    TARGET=$(stat -c %u:%g /open-gpu-kernel-modules)
    chown "$TARGET" /out
    exec gosu "$TARGET" "$0" "$@"
fi

cd /open-gpu-kernel-modules

# Debian splits kernel headers: source headers in -common, generated
# files (.config, include/generated) in the arch dir that
# /lib/modules/<ver>/build points to. conftest.sh needs both.
SYSOUT=$(readlink -f /lib/modules/*/build)
SYSSRC=$(ls -d /usr/src/linux-headers-*-common | head -n 1)
export SYSSRC SYSOUT

make clean
bear -- make modules -j"$(nproc)"

exec askl-clang-indexer /open-gpu-kernel-modules \
  --project open-gpu-kernel-modules \
  -j "$(nproc)" \
  --progress \
  --modules kbuild \
  -o /out \
  "$@"
