// Live texture replacements for both renderers (include/live_replacements.h).

#include "live_replacements.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "texture_upscale.h"

namespace rush2::live {

namespace {
    using rush2::upscale::Image;

    std::atomic<bool> g_fast3d{false};
    std::atomic<uint32_t> g_generation{1};
    std::mutex g_mutex;

    struct Entry {
        std::shared_ptr<const Image> image;
        RT64::ReplacementRegion region;
        uint32_t serial = 0;
    };
    std::unordered_map<uint64_t, Entry> g_entries;
    std::unordered_map<uint64_t, std::shared_ptr<const Image>> g_contents;
    uint32_t g_next_serial = 1;
}

void use_fast3d(bool on) { g_fast3d = on; }
bool fast3d() { return g_fast3d; }

bool add(uint64_t hash, uint64_t content, std::vector<uint8_t> file, RT64::ReplacementRegion region, bool centered) {
    if (!g_fast3d) return RT64::addLiveReplacement(hash, content, std::move(file), region, centered);

    std::shared_ptr<const Image> image;
    {
        std::lock_guard lock(g_mutex);
        auto c = g_contents.find(content);
        if (c != g_contents.end()) image = c->second;
    }
    if (!image) {
        if (file.empty()) return false;
        auto decoded = std::make_shared<Image>();
        if (!rush2::upscale::read_dds(file, *decoded) && !rush2::upscale::decode_image(file, *decoded)) return false;
        if (decoded->empty() || decoded->rgba.size() < size_t(decoded->width) * decoded->height * 4) return false;
        image = std::move(decoded);
    }
    {
        std::lock_guard lock(g_mutex);
        g_contents[content] = image;
        Entry& e = g_entries[hash];
        e.image = std::move(image);
        e.region = region;
        e.serial = g_next_serial++;
    }
    g_generation++;
    return true;
}

bool has_content(uint64_t content) {
    if (!g_fast3d) return RT64::hasLiveReplacementContent(content);
    std::lock_guard lock(g_mutex);
    return g_contents.count(content) != 0;
}

void remove(uint64_t hash) {
    if (!g_fast3d) return RT64::removeLiveReplacement(hash);
    {
        std::lock_guard lock(g_mutex);
        if (g_entries.erase(hash) == 0) return;
    }
    g_generation++;
}

void clear() {
    if (!g_fast3d) return RT64::clearLiveReplacements();
    {
        std::lock_guard lock(g_mutex);
        g_entries.clear();
        g_contents.clear();
    }
    g_generation++;
}

uint32_t generation() { return g_generation; }

int find(uint64_t hash, uint32_t width, uint32_t height, uint32_t* serial, std::vector<uint8_t>& rgba, uint32_t& w,
         uint32_t& h) {
    std::shared_ptr<const Image> image;
    RT64::ReplacementRegion region;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_entries.find(hash);
        if (it == g_entries.end()) return 0;
        if (*serial == it->second.serial) return 1;
        image = it->second.image;
        region = it->second.region;
        *serial = it->second.serial;
    }
    if (region.isWhole() || width == 0 || height == 0 || region.sourceWidth == 0 || region.sourceHeight == 0) {
        w = image->width;
        h = image->height;
        rgba = image->rgba;
        return 2;
    }
    // A strip of a bigger image: the part of it at (x, y) of the source's texels, width x height of them.
    const double sx = double(image->width) / region.sourceWidth;
    const double sy = double(image->height) / region.sourceHeight;
    const uint32_t x0 = std::min<uint32_t>(uint32_t(region.x * sx + 0.5), image->width - 1);
    const uint32_t y0 = std::min<uint32_t>(uint32_t(region.y * sy + 0.5), image->height - 1);
    w = std::max<uint32_t>(1, std::min<uint32_t>(uint32_t(width * sx + 0.5), image->width - x0));
    h = std::max<uint32_t>(1, std::min<uint32_t>(uint32_t(height * sy + 0.5), image->height - y0));
    rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        std::memcpy(&rgba[size_t(y) * w * 4], &image->rgba[(size_t(y0 + y) * image->width + x0) * 4], size_t(w) * 4);
    }
    return 2;
}

} // namespace rush2::live
