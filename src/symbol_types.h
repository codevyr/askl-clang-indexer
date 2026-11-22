#pragma once

// Scope constants (matching proto/index.proto SymbolScope enum)
constexpr int SCOPE_LOCAL = 1;
constexpr int SCOPE_GLOBAL = 2;

// Type constants (matching proto/index.proto SymbolType enum)
constexpr int SYMTYPE_FUNCTION = 1;
constexpr int SYMTYPE_FILE = 2;
constexpr int SYMTYPE_MODULE = 3;
constexpr int SYMTYPE_DIRECTORY = 4;
constexpr int SYMTYPE_TYPE = 5;
constexpr int SYMTYPE_DATA = 6;
constexpr int SYMTYPE_MACRO = 7;
constexpr int SYMTYPE_FIELD = 8;

// Instance type constants (matching proto/index.proto InstanceType enum)
constexpr int INSTTYPE_DEFINITION = 1;
constexpr int INSTTYPE_DECLARATION = 2;
constexpr int INSTTYPE_EXPANSION = 3;
constexpr int INSTTYPE_SENTINEL = 4;
constexpr int INSTTYPE_CONTAINMENT = 5;
constexpr int INSTTYPE_SOURCE = 6;
constexpr int INSTTYPE_HEADER = 7;
constexpr int INSTTYPE_BUILD = 8;
constexpr int INSTTYPE_FILE = 9;
constexpr int INSTTYPE_DOCUMENTATION = 10;

// What a referencing token does to its target (matching proto/index.proto
// RefRole): a SET of these bits, zero meaning nothing is stated.
constexpr unsigned REFROLE_CALL = 1;   // the token is the callee of a call
constexpr unsigned REFROLE_WRITE = 2;  // the target is assigned or initialised
constexpr unsigned REFROLE_TAKE = 4;   // its value or address is taken, not called

// What a row is when no token says it (matching proto/index.proto
// RefRelation).  Zero for a reference.
constexpr int REFRELATION_NONE = 0;
constexpr int REFRELATION_DECLARES = 1;  // the source's invocation declared the target
constexpr int REFRELATION_HOLDS = 2;     // the source is a slot and the target stored in it
