#pragma once

// Scope constants (matching proto/index.proto SymbolScope enum)
constexpr int SCOPE_LOCAL = 1;
constexpr int SCOPE_GLOBAL = 2;

// Type constants (matching proto/index.proto SymbolType enum)
constexpr int SYMTYPE_FUNCTION = 1;
constexpr int SYMTYPE_FILE = 2;
constexpr int SYMTYPE_DIRECTORY = 4;
constexpr int SYMTYPE_TYPE = 5;
constexpr int SYMTYPE_DATA = 6;
