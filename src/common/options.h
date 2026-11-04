#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "common/fov.h"

// MOHRS settings kept in the settings file beside the exe, beyond the SDK's own (after MOHRS-XBERecomp's)
namespace game {

struct Options {
    bool fps_counter = false;     // frames a second in the picture's top right corner
    uint32_t fov = kDefaultFov;   // horizontal degrees on a 4:3 frame; 70 original
    // keyboard and mouse (src/common/mouse_keyboard.cpp)
    float mouse_sensitivity = 1.0f;  // horizontal, 0.05 degrees a count at 1
    float mouse_vertical_sensitivity = 1.0f;
    bool invert_mouse = false;  // the Y axis
    bool invert_mouse_x = false;
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};

Options& Opt();

}  // namespace game
