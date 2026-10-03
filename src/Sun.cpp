#include "Sun.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include "Log.hpp"
#include "engine/map.hpp"
#include "engine/tag.hpp"
#include "memory/Memory.hpp"

namespace Sun {
    namespace {
        constexpr std::uint32_t fourcc(const char (&s)[5]) {
            return (std::uint32_t(s[0]) << 24) | (std::uint32_t(s[1]) << 16) | (std::uint32_t(s[2]) << 8) | std::uint32_t(s[3]);
        }

        // sky tag (Halo CE cache layout): ... shader functions, animations, lights reflexives.
        // A light is 116 bytes: lens flare reference, marker name, flags, color, power, test
        // distance, direction (yaw, pitch in radians), diameter.
        constexpr std::uintptr_t kSkyLights = 0xC4;
        constexpr std::uint32_t kLightSize = 116;
        constexpr std::uintptr_t kLightColor = 0x50, kLightPower = 0x5C, kLightDirection = 0x68;

        template <class T> bool read(std::uintptr_t at, T& out) {
            if (!at || !Memory::isAllocated(at) || !Memory::isAllocated(at + sizeof(T) - 1))
                return false;
            std::memcpy(&out, reinterpret_cast<const void*>(at), sizeof(T));
            return true;
        }

        Light find() {
            Light best;
            float bestScore = -1.0f;
            for (std::uint32_t i = 0; i < 0x10000; ++i) {
                Engine::Tag* tag = Engine::getTag(i);
                if (!tag || !Memory::isAllocated(reinterpret_cast<std::uintptr_t>(tag)))
                    break;
                if (tag->groupID != fourcc("sky ") || !Engine::tagExists(tag))
                    continue;
                const auto sky = reinterpret_cast<std::uintptr_t>(tag->getData());
                std::uint32_t count = 0, list = 0;
                if (!read(sky + kSkyLights, count) || !read(sky + kSkyLights + 4, list) || count == 0 || count > 16)
                    continue;
                const auto lights = Engine::translateMapAddress(list);
                for (std::uint32_t l = 0; l < count; ++l) {
                    const auto at = lights + l * kLightSize;
                    std::uint32_t flare = 0;
                    float color[3], power = 0.0f, angles[2];
                    if (!read(at, flare) || !read(at + kLightColor, color) || !read(at + kLightPower, power) || !read(at + kLightDirection, angles))
                        continue;
                    log(std::format("sky {} light {}: flare {:08x} color {:.2f} {:.2f} {:.2f} power {:.2f} yaw {:.1f} pitch {:.1f} deg",
                        tag->getResourcePath(), l, flare, color[0], color[1], color[2], power, angles[0] * 57.29578f, angles[1] * 57.29578f));
                    // The sun is the light with a lens flare; the strongest one if several.
                    const float score = (flare == fourcc("lens") ? 1e6f : 0.0f) + std::max(power, 0.0f);
                    if (!(std::isfinite(angles[0]) && std::isfinite(angles[1])) || score <= bestScore)
                        continue;
                    bestScore = score;
                    best.found = true;
                    best.power = power;
                    std::memcpy(best.color, color, sizeof(color));
                    const float cp = std::cos(angles[1]);
                    best.dir[0] = cp * std::cos(angles[0]);
                    best.dir[1] = cp * std::sin(angles[0]);
                    best.dir[2] = std::sin(angles[1]);
                }
            }
            return best;
        }

        std::string loadedFor;
        Light light;
    }

    const Light& current() {
        const char* map = Engine::getMapName();
        if (map && loadedFor != map) {
            loadedFor = map;
            light = find();
            log(light.found ? std::format("{}: sun at ({:.2f} {:.2f} {:.2f})", map, light.dir[0], light.dir[1], light.dir[2])
                            : std::format("{}: no sun in its sky", map));
        }
        return light;
    }
}
