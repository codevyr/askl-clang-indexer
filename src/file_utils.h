#pragma once

#include <string>

// Compute MIME-like filetype from file extension.
inline std::string computeFiletype(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "text/plain";
    auto ext = path.substr(dot);
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "text/x-c";
    if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx") return "text/x-c-header";
    return "text/plain";
}
