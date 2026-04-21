#!/bin/sh
set -eu

if [ "$(id -u)" = "0" ]; then
    TARGET=$(stat -c %u:%g /ompi)
    chown "$TARGET" /out
    exec gosu "$TARGET" "$0" "$@"
fi

cd /ompi

perl autogen.pl

./configure \
  --enable-debug \
  --enable-mem-debug \
  --enable-heterogeneous \
  --enable-ipv6 \
  --enable-mpi-fortran \
  --enable-mpi1-compatibility \
  --enable-oshmem \
  --enable-oshmem-fortran \
  --enable-spc \
  --enable-devel-headers \
  --with-devel-headers \
  --with-cuda=/usr \
  --with-rocm=/usr \
  --with-ucx \
  --with-ofi \
  --with-libnl \
  --with-valgrind \
  --with-cma \
  --with-libevent \
  --with-hwloc \
  --with-pmix=internal

bear -- make -j"$(nproc)"

exec askl-clang-indexer /ompi \
  --project ompi \
  -j "$(nproc)" \
  --progress \
  -o /out \
  "$@"
