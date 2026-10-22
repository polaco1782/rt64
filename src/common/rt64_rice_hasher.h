// SPDX-License-Identifier: GPL-2.0-only
// Rice/GLideN64 checksum implementation; see tools/texture_hasher/GLIDEN64-LICENSE.
#pragma once
#include "rt64_load_types.h"
#include <string>
#include <vector>
namespace RT64 {
std::string calculateRiceHash(const LoadTile& drawTile, const LoadOperation& loadOp, LoadTLUT drawTLUT,
                              const std::vector<uint8_t>& rdramBytes, const std::vector<uint8_t>& paletteBytes);
}
