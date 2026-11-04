// Mouse look: raw mouse counts turn the player's view directly, past the game's 8-bit stick
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "xdk/hle.h"

namespace {

// the player (sub_000DB530's esi, sub_000E0A50's): two sets of view angles, each a yaw in radians, positive left and
// wrapped to +-pi by the game after the hooks, and a pitch in radians, positive down
constexpr uint32_t kWalkYaw = 0x274;   // on foot, turned by sub_000E0A50 at the look rates sub_000DB530 set
constexpr uint32_t kWalkPitch = 0x278;
constexpr uint32_t kMountYaw = 0xE1C;  // on a mounted gun, turned in sub_000DB530 within its limits
constexpr uint32_t kMountPitch = 0xE20;
constexpr uint32_t kPlayerPort = 0x10DC;        // the controller port, byte
constexpr uint32_t kPlayerLookLocked = 0x1D14;  // byte, set: the mounted gun ignores the look stick
// radians a count at 100% sensitivity: 0.05 degrees
constexpr float kRadians = 0.05f * 3.14159265f / 180.0f;

float GetFloat(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
void SetFloat(uint32_t va, float f) { std::memcpy(kernel::HostPtr(va), &f, 4); }
uint8_t GetByte(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }

// the mouse's turn onto a yaw and pitch
void Turn(uint32_t p, uint32_t yaw, uint32_t pitch) {
    float dx;
    float dy;
    if (!input::TakeMouse(dx, dy)) return;
    // slower as far as sights and scopes zoom in
    float zoom = std::clamp(app::g_zoom, 0.05f, 1.0f);
    SetFloat(p + yaw, std::remainder(GetFloat(p + yaw) - dx * kRadians * zoom, 6.2831853f));
    SetFloat(p + pitch, GetFloat(p + pitch) + dy * kRadians * zoom);
}

}  // namespace

// sub_000E0A50, before it turns the player's view by the look rates and frame time
void WalkTurn(X86Context& ctx, uint8_t*) {
    uint32_t p = ctx.esi;
    if (GetByte(p + kPlayerPort) == 0) Turn(p, kWalkYaw, kWalkPitch);
}

// sub_000DB530 on a mounted gun, before it adds the stick's turn to the gun's view angles and clamps them
void MountLook(X86Context& ctx, uint8_t*) {
    uint32_t p = ctx.esi;
    if (GetByte(p + kPlayerPort) != 0) return;
    float dx;
    float dy;
    if (GetByte(p + kPlayerLookLocked)) input::TakeMouse(dx, dy);
    else Turn(p, kMountYaw, kMountPitch);
}

// The keys' game actions (Fire, Action, Crouch ...) go into the input manager's action array after sub_000B8820 maps
// the controllers into it, so the keys mean the same whichever controller layout the player picked. The array is per
// player: 0x271188 + player * 0x8F8, its first byte the controller port (0xFF none), the actions from +0x7C,
// 12 bytes each: flags byte (1 analog, 2 press edge, 4 signed), the last raw value at +4, the value read at +8.
namespace {
constexpr uint32_t kInput = 0x271188;
constexpr uint32_t kPlayerStride = 0x8F8;
constexpr uint32_t kActionArray = 0x7C;
constexpr uint32_t kActionSize = 12;
constexpr uint32_t kActionUseHeld = 178;  // Action's held twin, the same button in every layout
constexpr uint32_t kActionUse = 12;
uint32_t g_previous = 0;  // the keys' actions at the last update, for the press edges

void PressAction(uint32_t entry, bool now, bool before) {
    uint8_t flags = GetByte(entry);
    uint32_t value;
    if (flags & 2) {
        kernel::W32(entry + 4, now ? 1 : kernel::R32(entry + 4));
        value = now && !before;
    } else if (flags & 1) {
        value = flags & 4 ? 127 : 255;
    } else {
        value = 1;
    }
    if (now || value) kernel::W32(entry + 8, std::max(kernel::R32(entry + 8), value));
}
}  // namespace

void RefreshPrompts();  // prompts.cpp

X86_EXTERN_FUNC(__imp__sub_000B8820);
X86_FUNC(sub_000B8820) {
    __imp__sub_000B8820(ctx, base);
    RefreshPrompts();
    uint32_t held = input::GameActions();
    uint32_t before = g_previous;
    g_previous = held;
    for (uint32_t player = 0; (held || before) && player < 4; player++) {
        uint32_t object = kInput + player * kPlayerStride;
        if (GetByte(object) != 0) continue;  // the keyboard is controller port 0
        uint32_t array = object + kActionArray;
        for (uint32_t id = 0; id < 32; id++) {
            bool now = held >> id & 1, was = before >> id & 1;
            if (!now && !was) continue;
            PressAction(array + id * kActionSize, now, was);
            if (id == kActionUse) PressAction(array + kActionUseHeld * kActionSize, now, was);
        }
        break;
    }
    // MOHRS_LOG_ACTIONS: the live binding table and player 1's actions as they change, for finding action ids
    static const bool log_actions = std::getenv("MOHRS_LOG_ACTIONS") != nullptr;
    if (log_actions) {
        static std::string table_last;
        std::string table = "layout " + std::to_string(GetByte(kInput + 0x2402)) + ":";
        uint32_t t = kernel::R32(kInput + 0x23E4);
        for (uint32_t i = 0; t && i < 64; i++) {
            uint16_t a = kernel::R16(t + 4 * i), in = kernel::R16(t + 4 * i + 2);
            if (a == 0xFFFF) break;
            table += " " + std::to_string(a) + "<" + std::to_string(in);
        }
        if (table != table_last) LOGI("actions table %s", table.c_str());
        table_last = table;
        static std::string last;
        std::string now;
        for (uint32_t id = 0; id < 181; id++) {
            uint32_t v = kernel::R32(kInput + kActionArray + id * kActionSize + 8);
            if (v && id != 8 && id != 9) now += std::to_string(id) + "=" + std::to_string(int32_t(v)) + " ";
        }
        if (now != last) LOGI("actions: %s", now.c_str());
        last = now;
    }
}
