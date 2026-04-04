#pragma once

#include <string>
#include "symbol_types.h"

// Compute MIME-like filetype from file extension.
inline std::string computeFiletype(const std::string& path) {
    auto slash = path.rfind('/');
    auto base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (base == "Makefile" || base == "makefile" || base == "GNUmakefile" || base == "CMakeLists.txt")
        return "text/x-makefile";
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "text/plain";
    auto ext = path.substr(dot);
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "text/x-c";
    if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx") return "text/x-c-header";
    return "text/plain";
}

// Map MIME filetype to instance type for FILE instances.
inline int filetypeToInstType(const std::string& filetype) {
    if (filetype == "text/x-c") return INSTTYPE_SOURCE;
    if (filetype == "text/x-c-header") return INSTTYPE_HEADER;
    if (filetype == "text/x-makefile") return INSTTYPE_BUILD;
    return INSTTYPE_FILE;
}
