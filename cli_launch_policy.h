#pragma once

#include <cstring>

// Removed commands are rejected before help, singleton IPC or application
// initialization, including legacy aliases and unknown --kangaroo* variants.
inline bool cli_removed_kangaroo_requested(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--kangaroo", 10) == 0)
            return true;
    return false;
}

// Read-only help bypasses application initialization. All computational work
// still owns the normal run mutex and never requests that another run stop.
inline bool cli_help_requested(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            return true;
    return false;
}

inline bool cli_exclusive_diagnostic(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
        const char* option = argv[i];
        if (std::strcmp(option, "--test") == 0 ||
            std::strcmp(option, "--range-test") == 0 ||
            std::strcmp(option, "--strict-range-test") == 0 ||
            std::strcmp(option, "--bench") == 0)
            return true;
    }
    return false;
}
