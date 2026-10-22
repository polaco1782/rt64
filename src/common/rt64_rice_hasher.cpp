// SPDX-License-Identifier: GPL-2.0-only
// Derived from RT64's texture_hasher, GLideN64 and RiceVideo.
// See tools/texture_hasher/GLIDEN64-LICENSE for the original attribution.
#include "rt64_rice_hasher.h"
#include "rt64_replacement_database.h"
#include "shared/rt64_f3d_defines.h"
#include <algorithm>
#include <cstring>
namespace RT64 {
std::string calculateRiceHash(const LoadTile& drawTile, const LoadOperation& loadOp, LoadTLUT drawTLUT,
                              const std::vector<uint8_t>& rdramBytes, const std::vector<uint8_t>& paletteBytes) {
    if (!loadOp.texture.width || drawTile.siz > 3 || loadOp.tile.siz > 3 || loadOp.texture.siz > 3 ||
        drawTile.masks > 15 || drawTile.maskt > 15 || loadOp.tile.masks > 15 || loadOp.tile.maskt > 15)
        return {};
    // The code below has been referenced from GlideN64 and RiceVideo to imitate their hashing
    // algorithms. To comply with their licenses, the texture hasher tool is also under the GPL.
    auto CalculateDXT = [&](uint32_t txl2words) {
        if (txl2words == 0) {
            return 1U;
        } else {
            return (2048 + txl2words - 1) / txl2words;
        }
    };

    auto Txl2Words = [&](uint32_t width, uint32_t size) {
        static uint32_t sizeBytes[4] = {0, 1, 2, 4};
        if (size == 0) {
            return std::max(1U, width / 16);
        } else {
            return std::max(1U, width * sizeBytes[size] / 8);
        }
    };

    auto ReverseDXT = [&](uint32_t val, uint32_t width, uint32_t size) {
        if (val == 0x800) {
            return 1;
        }

        int low = 2047 / val;
        if (CalculateDXT(low) > val) {
            low++;
        }

        int high = 2047 / (val - 1);
        if (low == high) {
            return low;
        }

        for (int i = low; i <= high; i++) {
            if (Txl2Words(width, size) == (uint32_t)i) {
                return i;
            }
        }

        return (low + high) / 2;
    };

    int bpl;
    int width, height;
    if (loadOp.type == RT64::LoadOperation::Type::Tile) {
        uint16_t tileWidth = (std::max((loadOp.tile.lrs >> 2) - (loadOp.tile.uls >> 2), 0) + 1) & 0x03FF;
        uint16_t tileHeight = (std::max((loadOp.tile.lrt >> 2) - (loadOp.tile.ult >> 2), 0) + 1) & 0x03FF;
        tileWidth = (loadOp.tile.masks != 0) ? std::min(tileWidth, uint16_t(1U << loadOp.tile.masks)) : tileWidth;
        bpl = loadOp.texture.width << loadOp.texture.siz >> 1;
        width = std::min(tileWidth, loadOp.texture.width);
        if (loadOp.tile.siz > drawTile.siz) {
            width <<= loadOp.tile.siz - drawTile.siz;
        }

        height = (loadOp.tile.maskt != 0) ? std::min(tileHeight, uint16_t(1U << loadOp.tile.maskt)) : tileHeight;
    } else if (loadOp.type == RT64::LoadOperation::Type::Block) {
        int tile_width = std::max((drawTile.lrs >> 2) - (drawTile.uls >> 2), 0) + 1;
        int tile_height = std::max((drawTile.lrt >> 2) - (drawTile.ult >> 2), 0) + 1;
        int mask_width = (drawTile.masks == 0) ? (tile_width) : (1 << drawTile.masks);
        int mask_height = (drawTile.maskt == 0) ? (tile_height) : (1 << drawTile.maskt);
        bool clamps = (drawTile.masks == 0) || (drawTile.cms & G_TX_CLAMP);
        bool clampt = (drawTile.maskt == 0) || (drawTile.cmt & G_TX_CLAMP);
        if (clamps && (tile_width <= 256)) {
            width = std::min(mask_width, tile_width);
        } else {
            width = mask_width;
        }

        if ((clampt && (tile_height <= 256)) || (mask_height > 256)) {
            height = std::min(mask_height, tile_height);
        } else {
            height = mask_height;
        }

        if (drawTile.siz == G_IM_SIZ_32b)
            bpl = drawTile.line << 4;
        else if (loadOp.tile.lrt == 0) {
            bpl = drawTile.line << 3;
        } else {
            uint32_t dxt = loadOp.tile.lrt;
            if (dxt > 1) {
                dxt = ReverseDXT(dxt, loadOp.texture.width, loadOp.texture.siz);
            }

            bpl = dxt << 3;
        }
    } else {
        // Invalid case.
        return {};
    }

    auto CalculateMaxCI8b = [&](const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride) {
        uint8_t val = 0;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *buf = src + rowStride * y;
            for (uint32_t x = 0; x < width; ++x) {
                if (buf[x] > val) {
                    val = buf[x];
                }

                if (val == 0xFF) {
                    return uint8_t(0xFF);
                }
            }
        }

        return val;
    };

    auto CalculateMaxCI4b = [&](const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride) {
        uint8_t val = 0;
        uint8_t val1, val2;
        width >>= 1;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t *buf = src + rowStride * y;
            for (uint32_t x = 0; x < width; ++x) {
                val1 = buf[x] >> 4;
                val2 = buf[x] & 0xF;
                if (val1 > val) {
                    val = val1;
                }

                if (val2 > val) {
                    val = val2;
                }

                if (val == 0xF) {
                    return uint8_t(0xF);
                }
            }
        }

        return val;
    };

    auto RiceCRC32 = [](const uint8_t *src, int width, int height, int size, int rowStride) {
        uint32_t crc32Ret = 0;
        const uint32_t bytesPerLine = width << size >> 1;
        int y = height - 1;
        while (y >= 0) {
            uint32_t esi = 0;
            int x = bytesPerLine - 4;
            while (x >= 0) {
                memcpy(&esi, src + x, sizeof(esi));
                esi ^= x;

                crc32Ret = (crc32Ret << 4) + ((crc32Ret >> 28) & 15);
                crc32Ret += esi;
                x -= 4;
            }

            esi ^= y;
            crc32Ret += esi;
            src += rowStride;
            --y;
        }

        return crc32Ret;
    };

    // Rice expects to hash at least this many bytes, even if it leaks into other parts of RAM.
    // Zero stride is valid (notably single-row block loads). The byte-range
    // check below still covers every row visited by the checksum routine.
    if (width <= 0 || height <= 0 || bpl < 0) {
        fprintf(stderr, "Invalid Rice dimensions for %s.\n", "live texture");
        return {};
    }
    uint64_t expectedSize = uint64_t(height - 1) * bpl + (uint64_t(width) << drawTile.siz >> 1);
    if (expectedSize > rdramBytes.size()) {
        fprintf(stderr, "Unable to hash %s. Expected %llu bytes but got %zu bytes (%dX%d).\n", "live texture",
                static_cast<unsigned long long>(expectedSize), rdramBytes.size(), width, height);
        return {};
    }

    uint32_t riceCrc = RiceCRC32(rdramBytes.data(), width, height, drawTile.siz, bpl);
    RT64::ReplacementTexture replacement;
    replacement.hashes.rice = RT64::ReplacementDatabase::hashToString(riceCrc) + "#" + std::to_string(drawTile.fmt) +
                              "#" + std::to_string(drawTile.siz);

    // Add the palette hash if necessary.
    if (drawTLUT != RT64::LoadTLUT::None) {

        uint8_t cimax = 0;
        uint32_t ricePaletteCrc = 0;
        if (drawTile.siz == G_IM_SIZ_4b) {
            cimax = CalculateMaxCI4b(rdramBytes.data(), width, height, bpl);
            if (paletteBytes.size() < (uint32_t(cimax) + 1U) * 2U)
                return {};
            ricePaletteCrc = RiceCRC32(paletteBytes.data(), cimax + 1, 1, 2, 32);
        } else {
            cimax = CalculateMaxCI8b(rdramBytes.data(), width, height, bpl);
            if (paletteBytes.size() < (uint32_t(cimax) + 1U) * 2U)
                return {};
            ricePaletteCrc = RiceCRC32(paletteBytes.data(), cimax + 1, 1, 2, 512);
        }

        replacement.hashes.rice += "#" + RT64::ReplacementDatabase::hashToString(ricePaletteCrc);
    }

    return replacement.hashes.rice;
}
}
