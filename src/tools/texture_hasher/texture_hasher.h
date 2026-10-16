// SPDX-License-Identifier: GPL-2.0-only
// Rice/GLideN64 hashing exposed by RT64 for applications using the GPL.
#pragma once

#include <filesystem>

namespace RT64 {
    // Hash complete raw captures and atomically update directory/rt64.json.
    // Filesystem/database errors throw; individual invalid captures are skipped.
    void generateRiceHashes(const std::filesystem::path &directory);
}
