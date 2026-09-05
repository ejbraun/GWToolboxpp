#pragma once
#include <cmath>

namespace Tactical {
    struct Point { float x = 0.f, y = 0.f; };
    enum class Input { Passthrough, Map, Target };
    inline Input ResolveInput(const bool tactical, const bool map, const bool target)
    {
        return target ? Input::Target : (!tactical || map) ? Input::Map : Input::Passthrough;
    }
    struct View {
        Point center, screen;
        float scale = .05f, angle = 0.f;
        Point ToScreen(const Point world) const
        {
            const auto x = world.x - center.x, y = world.y - center.y;
            return {screen.x + (x * std::cos(angle) + y * std::sin(angle)) * scale,
                screen.y + (x * std::sin(angle) - y * std::cos(angle)) * scale};
        }
        Point ToWorld(const Point pixel) const
        {
            const auto x = (pixel.x - screen.x) / scale, y = (pixel.y - screen.y) / scale;
            return {center.x + x * std::cos(angle) + y * std::sin(angle),
                center.y + x * std::sin(angle) - y * std::cos(angle)};
        }
    };
}
