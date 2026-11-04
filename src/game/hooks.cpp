// Hooks for default.xbe
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

// the game's time step per vertical blank, in 1/60 s ticks: 1.0, 1.1988 in PAL mode (50 Hz). Callers read it
// before the limiter and store blanks * it as the frame's time step at 0x1FF528
constexpr uint32_t kStepPerBlank = 0x1FC678;
constexpr float kTicksPerSecond = 60.0f;
constexpr uint32_t kGameFps = 30;                 // the limiter waits out two blanks

float GetFloat(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
void SetFloat(uint32_t va, float f) { std::memcpy(kernel::HostPtr(va), &f, 4); }

// the game's own pacing at the 30 fps setting, the host's fps cap above it
bool Uncapped() { return app::Get().fps_cap != kGameFps; }

float g_blank = 0;    // the game's step per blank, as it last set it
float g_written = 0;  // what FrameTime last left in their place

}  // namespace

// before the limiter reads the blank count: true skips its wait
bool FrameWaitSkip(X86Context&, uint8_t*) { return Uncapped(); }

// before the limiter returns its blank count in eax
void FrameTime(X86Context& ctx, uint8_t*) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    auto now = clock::now();
    float elapsed = std::chrono::duration<float>(now - last).count();
    last = now;

    float current = GetFloat(kStepPerBlank);
    if (current != g_written || !g_blank) g_blank = current;  // first frame, or the game changed video mode
    if (!Uncapped()) {
        if (current != g_blank) SetFloat(kStepPerBlank, g_blank);
        g_written = g_blank;
        return;
    }
    // the next frame advances by the time this one took: at least 1/1000 s, at most four blanks as a long stall gave
    float dt = std::clamp(elapsed * kTicksPerSecond, 0.001f * kTicksPerSecond, g_blank * 4);
    SetFloat(kStepPerBlank, dt);
    g_written = dt;
    ctx.eax = 1;
}

// eax: the menus' whole step for this frame, truncated from the time step * 20; the dropped fraction carries over
void FrontEndStep(X86Context& ctx, uint8_t*) {
    static float carry = 0;
    constexpr uint32_t kStep = 0x1FF528;  // the frame's time step, in ticks
    constexpr float kUnitsPerTick = 20.0f;
    float units = GetFloat(kStep) * kUnitsPerTick + carry;
    int32_t whole = int32_t(units);
    carry = units - float(whole);
    ctx.eax = uint32_t(whole);
}

// the camera's half angles, on entry: [esp + 4] horizontal, [esp + 8] vertical, in radians
void FieldOfView(X86Context& ctx, uint8_t*) {
    // the gameplay cameras' calls: the player's view (0xA3AF6, 0xE3458) and the death camera (0xDAD41)
    uint32_t from = kernel::R32(ctx.esp);
    if (from != 0xA3AFB && from != 0xE345D && from != 0xDAD46) return;
    constexpr float kDeg = 3.14159265f / 180.0f;
    constexpr float kGameHalf = 35.0f * kDeg;  // half of game::kDefaultFov
    // the mouse turns slower as far as sights and scopes zoom in
    app::g_zoom = std::tan(GetFloat(ctx.esp + 4)) / std::tan(kGameHalf);
    uint32_t fov = game::Opt().fov;
    if (fov == game::kDefaultFov) return;
    constexpr float kZoomed = 20.0f * kDeg;    // at or below: the game's own zoom
    float k = std::tan(float(fov) * 0.5f * kDeg) / std::tan(kGameHalf);
    float x = GetFloat(ctx.esp + 4);
    // full at the game's view, none from kZoomed in, eased between so aiming down sights does not jump
    float t = std::clamp((x - kZoomed) / (kGameHalf - kZoomed), 0.0f, 1.0f);
    t = t * t * (3 - 2 * t);
    float scale = 1 + (k - 1) * t;
    SetFloat(ctx.esp + 4, std::atan(std::tan(x) * scale));
    SetFloat(ctx.esp + 8, std::atan(std::tan(GetFloat(ctx.esp + 8)) * scale));
}

// Movies (the EA logo, the opening cinematic, mission movies): the player calls sub_000A2560 every frame and stops
// when it returns 0. The game's own check skips only movies it marks skippable, and only on A or Start. A fresh
// left click, Enter, Space or Escape, or A, B, X or Y on any controller now skips every movie (Start still does
// through the game's check). Fresh: a press held from before the movie began waits for its release.
#include "common/mouse_keyboard.h"
#include "xdk/d3d8.h"

namespace {

// after the game's own check: al 0 stops the movie
void SkipCheck(X86Context& ctx) {
    static bool was = true;
    bool held = input::SkipHeld();
    bool press = held && !was;
    was = held;
    if (press) {
        LOGI("movie skipped");
        ctx.eax &= ~0xFFu;  // al 0: stop
        // the game draws nothing while it loads what comes next, and its presents would cycle the three frame buffers,
        // each left with another of the movie's pictures: the last picture stays up instead
        d3d::HoldFrame();
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000A2560);
X86_FUNC(sub_000A2560) {
    __imp__sub_000A2560(ctx, base);
    SkipCheck(ctx);
}

// The front end's movies (the attract movie the title plays when left idle: the EA logo and the opening cinematic)
// check with sub_001250A0, the same check without the skippable flag
X86_EXTERN_FUNC(__imp__sub_001250A0);
X86_FUNC(sub_001250A0) {
    __imp__sub_001250A0(ctx, base);
    SkipCheck(ctx);
}

// Loads (levels, and the menus after the startup movie): a loader thread (sub_000A1D20) runs the load's eleven steps,
// sleeping 20 ticks (200 ms) after each one, while the title thread waits for it. On the console the steps themselves
// took the time; here they take a few milliseconds, so the sleeps made every load about two seconds long, and the menus
// a second late after a skipped movie. The loader now sleeps one tick between steps.
X86_EXTERN_FUNC(__imp__sub_0016E170);
X86_FUNC(sub_0016E170) {
    constexpr uint32_t kLoaderReturn = 0xA1D60, kLoaderTicks = 0x14;
    if (kernel::R32(ctx.esp) == kLoaderReturn && kernel::R32(ctx.esp + 4) == kLoaderTicks) kernel::W32(ctx.esp + 4, 1);
    __imp__sub_0016E170(ctx, base);
}

// The game's menus with the mouse and PC Options in them, from MOHRS-XBERecomp (MIT, (c) 2026 neonoxd):
// src/common/menu_mouse.cpp, options_menu.cpp, options_package.cpp, feng.cpp
#include "common/menu_mouse.h"
#include "common/options_menu.h"

// PC Options in the options menus (src/common/options_menu.h): SetText 0x138A40, RunScript 0x138330, the menu sound
// 0x123C10
constexpr game::MenuCalls kMenuCalls = {0x138A40, 0x138330, 0x123C10};
// the open archive's file lookup, which the shell's package loader (0x122BC0) asks for each .fng
X86_EXTERN_FUNC(__imp__sub_001132E0);
X86_FUNC(sub_001132E0) { game::MenuFile(ctx, base, __imp__sub_001132E0); }
// the screen handler factory, by package name
X86_EXTERN_FUNC(__imp__sub_00120F00);
X86_FUNC(sub_00120F00) { game::MenuScreen(ctx, base, __imp__sub_00120F00); }
// the HUD options screen's event handler (vtable 0x1ED40C)
X86_EXTERN_FUNC(__imp__sub_0012A620);
X86_FUNC(sub_0012A620) { game::MenuEvent(ctx, base, __imp__sub_0012A620, kMenuCalls); }

// FEng menu pointer adapter: the actual text and sprite draws supply the clickable geometry
constexpr game::MenuMouseCalls kMouseMenu = {
    0x96900, 0x93260, 0x958D0, 0x162B20, 0x88630,
    0x1FC680, 0x1FC684, 0x246450, 0x246454, 0x246424, 0x246420, 0x271188};
X86_EXTERN_FUNC(__imp__sub_00095220);
X86_FUNC(sub_00095220) { game::MenuMouseUpdate(ctx, base, __imp__sub_00095220, kMouseMenu); }
X86_EXTERN_FUNC(__imp__sub_00093EE0);
X86_FUNC(sub_00093EE0) { game::MenuMouseRender(ctx, base, __imp__sub_00093EE0, kMouseMenu); }
X86_EXTERN_FUNC(__imp__sub_001164E0);
X86_FUNC(sub_001164E0) { game::MenuMouseObject(ctx, base, __imp__sub_001164E0); }
// ShellManager overrides the base FengInterface drawing callback; both shell and pause use this vtable slot
X86_EXTERN_FUNC(__imp__sub_00125E10);
X86_FUNC(sub_00125E10) { game::MenuMouseObject(ctx, base, __imp__sub_00125E10); }
X86_EXTERN_FUNC(__imp__sub_00162AE0);
X86_FUNC(sub_00162AE0) { game::MenuMouseText(ctx, base, __imp__sub_00162AE0, kMouseMenu); }
X86_EXTERN_FUNC(__imp__sub_000B6B90);
X86_FUNC(sub_000B6B90) { game::MenuMouseSprite(ctx, base, __imp__sub_000B6B90, kMouseMenu); }
