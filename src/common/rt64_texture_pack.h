// SPDX-License-Identifier: GPL-2.0-only
// Live Rice/GLideN64 ZIP matching. See tools/texture_hasher/GLIDEN64-LICENSE.
#pragma once
#include "rt64_load_types.h"
#include "rt64_replacement_database.h"
#include <functional>
#include <memory>

namespace RT64 {
// Owns the matching worker; the indexed ZIP remains owned by TextureCache.
// Construction indexes filenames and loads the sidecar, without decoding images.
class TexturePackScanner {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    TexturePackScanner(FileSystem *source, const std::filesystem::path& zipPath, const std::string& romTitle,
                       size_t rdramByteCount, std::function<void(const ReplacementTexture&)> matched,
                       bool logLoads = false);
    ~TexturePackScanner();
    const ReplacementDatabase& database() const; // Before start() only.
    void start();
    void stop();
    // Called while guest memory is stable. Bounded copies only; contention retries next use.
    void capture(uint64_t hash, uint64_t frame, const uint8_t *rdram, const LoadTile& tile, const LoadOperation& load,
                 const LoadOperation& palette, uint16_t width, uint16_t height, uint32_t tlut);
    // Called only by RT64's texture streaming workers. RGB/mask composition stays in memory.
    bool loadRGBA(const std::string& path, std::vector<uint8_t>& pixels, int& width, int& height) const;
};
}
