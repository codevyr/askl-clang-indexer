# askl-clang-indexer

C language indexer for the [askl](https://github.com/Codevyr/askl) code search engine. Parses C source code using libclang and produces a protobuf index file compatible with the askl server.

## Building

```bash
mkdir build && cd build
cmake .. \
  -Dprotobuf_DIR=/path/to/protobuf/lib/cmake/protobuf \
  -Dabsl_DIR=/path/to/abseil/lib/cmake/absl \
  -DLIBCLANG_PATH=/path/to/libclang/lib \
  -DLIBCLANG_INCLUDE_PATH=/path/to/clang-c/include
make -j$(nproc)
```

### Dependencies

- CMake >= 3.16
- libclang (Clang >= 21)
- protobuf (with abseil)
- C++17 compiler

## Testing

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --verbose
```

Tests use Google Test (fetched automatically via CMake FetchContent). Fixture directories under `test/` contain plain C source files — `compile_commands.json` is generated at runtime by the test harness.

## Usage

```bash
# Generate compile_commands.json for your project (e.g., using bear or cmake)
bear -- make

# Run the indexer
askl-clang-indexer \
  --compile-commands /path/to/project \
  --root /path/to/project \
  --project myproject \
  -o index.pb \
  -j 8

# Verify output
protoc --decode=askl.index.Project proto/index.proto < index.pb
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--compile-commands` | `.` | Directory containing `compile_commands.json` |
| `--output`, `-o` | `index.pb` | Output protobuf file path |
| `--project` | `main` | Project name |
| `--root` | auto-detect | Project root directory |
| `--threads`, `-j` | CPU count | Number of parallel threads |

## Docker

### Base indexer image

Builds the indexer binary in a multi-stage Docker build:

```bash
docker build -f docker/Dockerfile -t askl-clang-indexer:latest .
docker run --rm askl-clang-indexer:latest --help
```

Index a project with an existing `compile_commands.json`:

```bash
docker run --rm \
  -v /path/to/project:/project:ro \
  -v /path/to/output:/out \
  askl-clang-indexer:latest /project --project myproject -o /out
```

### Target-specific images

The `docker/` directory contains subdirectories for building indexes of specific software projects. Each extends the base indexer image with the target's build dependencies and an entrypoint that configures, builds, and indexes the project.

#### `docker/linux/` — Linux kernel

Builds on top of `askl-clang-indexer:latest`. Installs kernel build dependencies, then runs `make allmodconfig`, builds the kernel, generates `compile_commands.json`, and runs the indexer.

```bash
# Build the base image first
docker build -f docker/Dockerfile -t askl-clang-indexer:latest .

# Build the linux indexer image
docker build -f docker/linux/Dockerfile -t askl-linux-indexer:latest docker/linux/

# Index a kernel tree (sources are bind-mounted read-write for the build)
docker run --rm \
  -v /path/to/linux:/linux \
  -v /path/to/output:/out \
  askl-linux-indexer
```

## Architecture

- **Stage 1**: Per-TU AST walk extracting symbols (functions, structs, typedefs, enums, variables), symbol instances, and direct references (calls, type refs, decl refs).
- **Stage 2**: Function pointer assignment analysis — detects designated initializer patterns (`.read = my_read`) and direct assignment patterns (`ops->read = my_read`).

## License

See [LICENSE](LICENSE).
