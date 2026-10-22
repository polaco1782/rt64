// SPDX-License-Identifier: GPL-2.0-only
// Live Rice/GLideN64 matching; preserves the v64recomp scanner's filename rules.
#include "rt64_texture_pack.h"
#include "rt64_rice_hasher.h"
#include "shared/rt64_f3d_defines.h"
#include "xxHash/xxh3.h"
#include "rt64_tmem_hasher.h"
#include "stb/stb_image.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <unordered_set>
#ifdef __linux__
#include <sys/resource.h>
#endif
namespace fs = std::filesystem;
namespace RT64 {
namespace {
std::string lower(std::string value) {
    return ReplacementDatabase::toLower(std::move(value));
}
std::vector<uint8_t> readImage(FileSystem& source, const fs::path& path) {
    const auto name = path.generic_u8string();
    const auto size = source.getSize(name);
    if (!size || size > 256 * 1024 * 1024)
        throw std::runtime_error("invalid ZIP texture size: " + name);
    std::vector<uint8_t> bytes;
    if (!source.load(name, bytes))
        throw std::runtime_error("cannot read ZIP texture: " + name);
    return bytes;
}
struct Image {
    fs::path path, alpha;
    bool alternate = false, rgb = false;
    // Complete RGB/mask pairs take precedence over combined files. A combined
    // image retains transparency when a redundant, unpaired _rgb also exists.
    int priority() const {
        return !alpha.empty() ? 0 : rgb ? 2 : 1;
    }
};
struct PreparedImage {
    int width = 0, height = 0, priority = 1;
    std::vector<uint8_t> pixels;
};
std::vector<uint8_t> decode(const std::vector<uint8_t>& bytes, int& width, int& height) {
    int channels;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 8192 || height > 8192)
        throw std::runtime_error("invalid texture PNG or dimensions");
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!decoded)
        throw std::runtime_error("cannot decode texture PNG");
    return {decoded.get(), decoded.get() + size_t(width) * size_t(height) * 4};
}
PreparedImage prepareBytes(std::vector<uint8_t> bytes, int priority) {
    PreparedImage result;
    result.priority = priority;
    result.pixels = decode(bytes, result.width, result.height);
    return result;
}
PreparedImage prepareImage(FileSystem& source, const Image& image) {
    auto result = prepareBytes(readImage(source, image.path), image.priority());
    if (image.rgb) {
        if (!image.alpha.empty()) {
            int width, height;
            auto alpha = decode(readImage(source, image.alpha), width, height);
            if (width != result.width || height != result.height)
                throw std::runtime_error("RGB/alpha dimensions differ: " + image.path.string());
            // Rice's _a mask uses its red component, including when the mask
            // itself is RGB/RGBA. Its own alpha channel is not the mask.
            for (size_t pixel = 0; pixel < result.pixels.size(); pixel += 4)
                result.pixels[pixel + 3] = alpha[pixel];
        } else {
            for (size_t pixel = 3; pixel < result.pixels.size(); pixel += 4)
                result.pixels[pixel] = 255;
        }
    }
    return result;
}
using ImageIndex = std::map<std::string, std::vector<Image>>;
ImageIndex indexImages(FileSystem& source, const std::string& romTitle) {
    static const std::regex name(
        "([^#]+)#([0-9a-f]{8}|\\$)#([0-4])#([0-3])(?:#([0-9a-f]{8}|\\$))?_(all|ciByRGBA|allciByRGBA|rgb|a)\\.png",
        std::regex::icase);
    static const std::regex alternate("^(?:(?:copy of|flash)\\s+)+", std::regex::icase);
    struct Components {
        std::string key;
        fs::path combined, rgb, alpha;
        bool alternate = false;
        int combinedPriority = 3;
    };
    std::map<fs::path, Components> components;
    ImageIndex index;
    for (const auto& fileString : source) {
        const auto file = fs::u8path(fileString);
        std::smatch match;
        std::string filename = file.filename().string();
        if (!std::regex_match(filename, match, name))
            continue;
        std::string prefix = match[1].str();
        bool isAlternate = std::regex_search(prefix, alternate);
        prefix = std::regex_replace(prefix, alternate, "");
        if (lower(prefix) != lower(romTitle))
            continue;
        std::string key = lower(match[2].str()) + "#" + match[3].str() + "#" + match[4].str();
        if (match[5].matched)
            key += "#" + lower(match[5].str());
        auto stem = lower(filename.substr(0, filename.find_last_of('_')));
        auto& parts = components[file.parent_path() / stem];
        parts.key = key;
        parts.alternate = isAlternate;
        auto kind = lower(match[6].str());
        if (kind == "rgb")
            parts.rgb = file;
        else if (kind == "a")
            parts.alpha = file;
        else {
            int priority = kind == "all" ? 0 : kind == "cibyrgba" ? 1 : 2;
            if (priority < parts.combinedPriority) {
                parts.combined = file;
                parts.combinedPriority = priority;
            }
        }
    }
    for (const auto& [base, parts] : components) {
        (void)base;
        if (!parts.rgb.empty() && !parts.alpha.empty())
            index[parts.key].push_back({parts.rgb, parts.alpha, parts.alternate, true});
        else if (!parts.combined.empty())
            index[parts.key].push_back({parts.combined, {}, parts.alternate, false});
        else if (!parts.rgb.empty())
            index[parts.key].push_back({parts.rgb, {}, parts.alternate, true});
    }
    return index;
}
std::string matchKey(const ImageIndex& images, const std::string& rice) {
    auto contains = [&](const std::string& key) { return images.count(key); };
    if (contains(rice))
        return rice;
    static const std::regex ci("([0-9a-f]{8})#2#([0-3])#([0-9a-f]{8})");
    std::smatch fields;
    if (std::regex_match(rice, fields, ci)) {
        auto paletteWildcard = fields[1].str() + "#2#" + fields[2].str() + "#$";
        if (contains(paletteWildcard))
            return paletteWildcard;
        auto textureWildcard = "$#2#" + fields[2].str() + "#" + fields[3].str();
        if (contains(textureWildcard))
            return textureWildcard;
    }
    return {};
}
Image chooseImage(FileSystem& source, std::vector<Image> images) {
    bool ordinary = std::any_of(images.begin(), images.end(), [](const Image& image) { return !image.alternate; });
    if (ordinary)
        images.erase(std::remove_if(images.begin(), images.end(), [](const Image& image) { return image.alternate; }),
                     images.end());
    int priority = std::min_element(images.begin(), images.end(), [](const Image& a, const Image& b) {
                       return a.priority() < b.priority();
                   })->priority();
    images.erase(std::remove_if(images.begin(), images.end(),
                                [priority](const Image& image) { return image.priority() != priority; }),
                 images.end());
    std::sort(images.begin(), images.end(), [](const Image& a, const Image& b) { return a.path < b.path; });
    auto selected = prepareImage(source, images.front());
    for (size_t i = 1; i < images.size(); i++) {
        auto other = prepareImage(source, images[i]);
        if (other.width != selected.width || other.height != selected.height || other.pixels != selected.pixels)
            throw std::runtime_error("conflicting Rice images: " + images.front().path.string());
    }
    return images.front();
}
constexpr size_t maxCaptureBytes = 1024 * 1024, maxQueuedBytes = 8 * 1024 * 1024;
constexpr size_t maxTaskBytes = 256 * 1024, maxTaskCaptures = 8;
struct Range {
    uint64_t start, count;
    bool valid(size_t size) const {
        return count && count <= maxCaptureBytes && start < size && count <= size - start;
    }
};
Range textureRange(const LoadOperation& load, const LoadTile& tile, uint16_t width, uint16_t height) {
    uint64_t offset = uint64_t(load.tile.uls >> 2) << load.texture.siz >> 1;
    uint64_t rowBytes = uint64_t(load.texture.width) << load.texture.siz >> 1;
    Range range{load.texture.address, 0};
    if (load.type == LoadOperation::Type::Block) {
        range.start += offset + rowBytes * load.tile.ult;
        range.count = uint64_t(((load.tile.lrs - load.tile.uls) >> (4 - load.tile.siz)) + 1) << 3;
        rowBytes = std::max(rowBytes, uint64_t(tile.line) << 3);
    } else if (load.type == LoadOperation::Type::Tile) {
        range.start += offset + rowBytes * (load.tile.ult >> 2);
        range.count = uint64_t(1 + ((load.tile.lrt >> 2) - (load.tile.ult >> 2))) * rowBytes;
    }
    range.count = std::max(range.count, std::max(uint64_t(width) << tile.siz >> 1, rowBytes) * height);
    return range;
}
Range paletteRange(const LoadOperation& load) {
    uint64_t rowBytes = uint64_t(load.texture.width) << load.texture.siz >> 1;
    uint64_t rows = 1 + ((load.tile.lrt >> 2) - (load.tile.ult >> 2));
    uint64_t words = ((load.tile.lrs >> 2) - (load.tile.uls >> 2)) + 1;
    return {load.texture.address + (uint64_t(load.tile.uls >> 2) << load.texture.siz >> 1) +
                rowBytes * (load.tile.ult >> 2),
            (rows - 1) * rowBytes + (words << 3)};
}

}
struct TexturePackScanner::Impl {
    struct Capture {
        uint64_t hash;
        LoadTile tile;
        LoadOperation load;
        uint32_t tlut;
        std::vector<uint8_t> rdram, palette;
    };
    FileSystem *source;
    fs::path sidecar;
    std::string title, stamp;
    size_t rdramByteCount;
    bool logLoads;
    ImageIndex images;
    std::map<std::string, Image> paths;
    std::map<std::string, Image> chosen;
    std::set<std::string> rejected;
    ReplacementDatabase db;
    std::function<void(const ReplacementTexture&)> matched;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Capture> queue;
    std::unordered_set<uint64_t> seen;
    size_t queuedBytes = 0, taskBytes = 0, taskCount = 0;
    uint64_t taskFrame = UINT64_MAX;
    bool stopping = false, dirty = false;

    Impl(FileSystem *source, const fs::path& zip, const std::string& title, size_t rdramByteCount,
         std::function<void(const ReplacementTexture&)> matched, bool logLoads)
        : source(source), sidecar(zip.u8string() + ".rt64.json"), title(title), rdramByteCount(rdramByteCount),
          logLoads(logLoads), images(indexImages(*source, title)), matched(std::move(matched)) {
        stamp = std::to_string(fs::file_size(zip)) + ":" +
                std::to_string(fs::last_write_time(zip).time_since_epoch().count());
        db.config.hashVersion = TMEMHasher::CurrentHashVersion;
        db.config.defaultOperation = ReplacementOperation::Stream;
        for (const auto& [key, variants] : images)
            for (const auto& image : variants)
                paths.emplace(image.path.generic_u8string(), image);
        // A stale or invalid cache is disposable. Never trust mappings from another pack/title/version.
        if (fs::exists(sidecar)) {
            try {
                std::ifstream input(sidecar);
                json root;
                input >> root;
                if (root.at("source").at("stamp") == stamp && root.at("source").at("romTitle") == title) {
                    ReplacementDatabase cached = root;
                    if (cached.config.hashVersion == TMEMHasher::CurrentHashVersion) {
                        for (auto texture : cached.textures) {
                            if (!paths.count(texture.path) || texture.hashes.rt64.size() != 16 ||
                                texture.hashes.rt64.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                                continue;
                            texture.operation = ReplacementOperation::Stream;
                            db.addReplacement(texture);
                            seen.insert(ReplacementDatabase::stringToHash(texture.hashes.rt64));
                        }
                    }
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "RT64 ZIP mapping cache ignored: %s\n", e.what());
            }
        }
        fprintf(stderr, "RT64 ZIP: %zu Rice IDs, %zu cached mappings; index %s\n", images.size(), db.textures.size(),
                sidecar.u8string().c_str());
        if (images.empty())
            throw std::runtime_error("ZIP contains no supported Rice PNG filenames for ROM title: " + title);
    }
    void save() {
        json root = db;
        root["source"] = {{"stamp", stamp}, {"romTitle", title}};
        auto temporary = sidecar;
        temporary += ".tmp";
        std::ofstream output(temporary);
        output << root.dump(2) << '\n';
        output.close();
        if (!output)
            throw std::runtime_error("cannot write RT64 ZIP mapping: " + temporary.u8string());
        fs::rename(temporary, sidecar);
        dirty = false;
    }
    void process(const Capture& capture) {
        const auto tlut = capture.tlut == G_TT_RGBA16 ? LoadTLUT::RGBA16
                          : capture.tlut == G_TT_IA16 ? LoadTLUT::IA16
                                                      : LoadTLUT::None;
        const auto rice = calculateRiceHash(capture.tile, capture.load, tlut, capture.rdram, capture.palette);
        const auto key = matchKey(images, rice);
        if (key.empty() || rejected.count(key))
            return;
        auto it = chosen.find(key);
        if (it == chosen.end()) {
            try {
                it = chosen.emplace(key, chooseImage(*source, images.at(key))).first;
            } catch (const std::exception& e) {
                rejected.insert(key);
                fprintf(stderr, "RT64 ZIP match rejected: %s\n", e.what());
                return;
            }
        }
        ReplacementTexture texture;
        texture.hashes = {ReplacementDatabase::hashToString(capture.hash), rice};
        texture.path = it->second.path.generic_u8string();
        texture.operation = ReplacementOperation::Stream;
        db.addReplacement(texture);
        dirty = true;
        matched(texture);
    }
    void run() {
#ifdef __linux__
        setpriority(PRIO_PROCESS, 0, 10);
#endif
        auto nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;) {
            Capture capture;
            bool haveCapture = false, finish = false;
            {
                std::unique_lock lock(mutex);
                if (dirty)
                    changed.wait_until(lock, nextSave, [this] { return stopping || !queue.empty(); });
                else
                    changed.wait(lock, [this] { return stopping || !queue.empty(); });
                if (!queue.empty()) {
                    capture = std::move(queue.front());
                    queue.pop_front();
                    queuedBytes -= capture.rdram.size() + capture.palette.size();
                    haveCapture = true;
                }
                finish = stopping && queue.empty();
            }
            try {
                if (haveCapture)
                    process(capture);
                if (dirty && (finish || std::chrono::steady_clock::now() >= nextSave)) {
                    save();
                    nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "RT64 ZIP scanner: %s\n", e.what());
                nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            if (finish)
                return;
        }
    }
};
TexturePackScanner::TexturePackScanner(FileSystem *source, const fs::path& zipPath, const std::string& romTitle,
                                       size_t rdramByteCount, std::function<void(const ReplacementTexture&)> matched,
                                       bool logLoads)
    : impl(std::make_unique<Impl>(source, zipPath, romTitle, rdramByteCount, std::move(matched), logLoads)) {}
TexturePackScanner::~TexturePackScanner() {
    stop();
}
void TexturePackScanner::stop() {
    if (impl->worker.joinable()) {
        {
            std::lock_guard lock(impl->mutex);
            impl->stopping = true;
        }
        impl->changed.notify_one();
        impl->worker.join();
    }
}
const ReplacementDatabase& TexturePackScanner::database() const {
    return impl->db;
}
void TexturePackScanner::start() {
    impl->worker = std::thread(&Impl::run, impl.get());
}
void TexturePackScanner::capture(uint64_t hash, uint64_t frame, const uint8_t *rdram, const LoadTile& tile,
                                 const LoadOperation& load, const LoadOperation& palette, uint16_t width,
                                 uint16_t height, uint32_t tlut) {
    std::unique_lock lock(impl->mutex, std::try_to_lock);
    if (!lock || impl->stopping || impl->seen.count(hash))
        return;
    if (impl->taskFrame != frame) {
        impl->taskFrame = frame;
        impl->taskBytes = impl->taskCount = 0;
    }
    if (impl->taskCount >= maxTaskCaptures || impl->taskBytes >= maxTaskBytes)
        return;
    if (!load.texture.width || (tlut && !palette.texture.width))
        return;
    if (!width || !height || tile.siz > 3 || load.texture.siz > 3 || load.tile.siz > 3 ||
        (load.type != LoadOperation::Type::Tile && load.type != LoadOperation::Type::Block) ||
        load.tile.lrs < load.tile.uls || load.tile.lrt < load.tile.ult ||
        (tlut &&
         (palette.texture.siz > 3 || palette.tile.lrs < palette.tile.uls || palette.tile.lrt < palette.tile.ult)))
        return;
    auto texture = textureRange(load, tile, width, height);
    auto colors = tlut ? paletteRange(palette) : Range{0, 0};
    if (!texture.valid(impl->rdramByteCount) || (tlut && !colors.valid(impl->rdramByteCount)))
        return;
    const auto bytes = texture.count + colors.count;
    if (impl->queuedBytes + bytes > maxQueuedBytes || impl->queue.size() >= 64)
        return;
    Impl::Capture capture{hash, tile, load, tlut, {}, {}};
    capture.rdram.assign(rdram + texture.start, rdram + texture.start + texture.count);
    if (tlut)
        capture.palette.assign(rdram + colors.start, rdram + colors.start + colors.count);
    impl->queue.push_back(std::move(capture));
    impl->seen.insert(hash);
    impl->queuedBytes += bytes;
    impl->taskBytes += bytes;
    impl->taskCount++;
    lock.unlock();
    impl->changed.notify_one();
}
bool TexturePackScanner::loadRGBA(const std::string& path, std::vector<uint8_t>& pixels, int& width,
                                  int& height) const {
    try {
        auto it = impl->paths.find(path);
        if (it == impl->paths.end())
            return false;
        auto image = prepareImage(*impl->source, it->second);
        width = image.width;
        height = image.height;
        pixels = std::move(image.pixels);
        if (impl->logLoads) {
            const auto alpha = it->second.alpha.generic_u8string();
            fprintf(stderr, "RT64 ZIP loaded: %s (%dx%d)%s%s\n", path.c_str(), width, height,
                    alpha.empty() ? "" : "; alpha: ", alpha.c_str());
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "RT64 ZIP image: %s\n", e.what());
        return false;
    }
}
}
