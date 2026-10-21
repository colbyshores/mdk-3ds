// ============================================================================
// bni.cpp — see bni.h.
// ============================================================================

#include "formats/bni.h"

#include <cstddef>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

bool Bni::parse(Bni* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    Bni bni;
    if (!ArchiveIndex::read(&bni.index_, bytes, byteCount, 12)) {
        return false;
    }
    *out = std::move(bni);
    return true;
}

bool Bni::parse(Bni* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    Bni bni;
    if (!ArchiveIndex::read(&bni.index_, std::move(bytes), 12)) {
        return false;
    }
    *out = std::move(bni);
    return true;
}

bool Bni::has(const std::string& name) const {
    return index_.find(name) != nullptr;
}

void Bni::add(const std::string& name, const uint8_t* data, int32_t size) {
    index_.add(name, data, size);

    // The entry changed, so what was decoded from it is stale.
    for (size_t i = 0; i < images_.size(); i++) {
        if (images_[i].first == name) {
            images_.erase(images_.begin() + (std::ptrdiff_t)i);
            break;
        }
    }
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].first == name) {
            animations_.erase(animations_.begin() + (std::ptrdiff_t)i);
            break;
        }
    }
}

bool Bni::image(const std::string& name, Texture* out) const {
    if (!out) {
        return false;
    }
    for (size_t i = 0; i < images_.size(); i++) {
        if (images_[i].first == name) {
            *out = images_[i].second;
            return true;
        }
    }

    const ArchiveIndex::Entry* entry = index_.find(name);
    if (!entry) {
        return false;
    }
    const std::vector<uint8_t>& bytes = index_.bytes();
    if (entry->offset < 0 || entry->offset + 4 > (int32_t)bytes.size()) {
        return false;
    }

    Texture image = Texture::parse(name, bytes.data(), (int32_t)bytes.size(), entry->offset);
    images_.push_back(std::make_pair(name, image));
    *out = image;
    return true;
}

bool Bni::animation(const std::string& name, SpriteAnimation* out) const {
    if (!out) {
        return false;
    }
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].first == name) {
            *out = animations_[i].second;
            return true;
        }
    }

    const ArchiveIndex::Entry* entry = index_.find(name);
    if (!entry) {
        return false;
    }
    const std::vector<uint8_t>& bytes = index_.bytes();
    // The entry starts with the animation's size.
    if (entry->offset < 0 || entry->offset + 4 > (int32_t)bytes.size()) {
        return false;
    }

    SpriteAnimation animation;
    if (!SpriteAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), entry->offset + 4)) {
        return false;
    }
    animations_.push_back(std::make_pair(name, animation));
    *out = animation;
    return true;
}

} // namespace formats
} // namespace mdk
