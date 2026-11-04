#include "common/menu_mouse.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif
#include "common/feng.h"
#include "common/menu_pointer.h"
#include "common/mouse_keyboard.h"
#include "common/options_package.h"
#include "core/diag.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
namespace {
using kernel::R32;
using kernel::G;
using menu::Rect;
using Clock = std::chrono::steady_clock;
std::atomic<int64_t> cursor_rendered_ms{0};
constexpr int64_t kCursorFreshMs = 100;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
constexpr uint32_t kPressed = 0x0C407210, kReleased = 0x936A6A7F, kAccept = 0x406415E3;
constexpr uint32_t kBack = 0x911AB364, kLeft = 0x9120409E, kRight = 0xB5971BF1;
constexpr uint32_t kPackage = 0xFFFFFFFD, kHandler = 0xFFFFFFFB;
constexpr uint32_t kButton = 0x10000000, kDisabled = 0x04000000;

enum class Action { None, Activate, Focus, Left, Right, Accept, Back, Resume, Tab0, Tab1, Tab2, Tab3, Mission };
struct Object {
    uint32_t package = 0, button = 0, hash = 0, guid = 0;
    bool disabled = false;
    uint32_t address = 0, button_guid = 0;
    bool shown_only = false;  // a package drawn without taking this player's input: seen, never clicked
};
struct Region {
    Object object;
    Rect rect;
    Action action = Action::None;
};
struct Frame {
    std::string name;
    bool shown_only = false;  // an overlay beside the menu, such as the save screen's bar
    std::vector<Region> regions;
};
struct State {
    uint32_t engine = 0, drawing = 0;
    std::unordered_map<uint32_t, Object> objects;
    std::unordered_map<uint32_t, Frame> frames;
    bool rendering = false;
    uint32_t object_draws = 0, known_draws = 0, text_draws = 0, sprite_draws = 0;
    Clock::time_point rendered{};
    menu::PointerButtons buttons;
    uint32_t active = 0, press_package = 0, press_object = 0;
    uint32_t input_mask = 0;
    uint32_t press_guid = 0;
    std::string active_name;
    bool foreground = false;
    float x = -1, y = -1, width = 0, height = 0;
    uint32_t tab_package = 0, tab_message = 0;
    std::string tab_name;
};
thread_local State state;

bool Trace() {
    static const bool on = std::getenv("MOHRS_MENU_MOUSE_TRACE") != nullptr;
    return on;
}

bool Readable(uint32_t va, uint32_t size = 4) {
    return va >= 0x10000 && va <= UINT32_MAX - size && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

uint32_t KeyboardMask(const MenuMouseCalls& calls) {
    // FEng masks address logical players, while KBM is always guest port 0 (docs/controller-routing.md).
    if (!Readable(calls.input_manager, 0x23E1)) return 0;
    uint32_t count = std::min(uint32_t(G<uint8_t>(calls.input_manager + 0x23E0)), 4u);
    for (uint32_t i = 0; i < count; i++)
        if (G<uint8_t>(calls.input_manager + i * 0x8F8) == 0) return 1u << i;
    return 0;
}

std::string Name(uint32_t package) {
    uint32_t name = R32(package + 0x0C);
    return Readable(name, 64) ? kernel::ReadCString(name, 63) : std::string();
}

int Tab(const std::string& name) {
    if (_stricmp(name.c_str(), "ObjectivesOverlay.fng") == 0 || _stricmp(name.c_str(), "MPLeaderBoard.fng") == 0) return 0;
    if (_stricmp(name.c_str(), "Notes.fng") == 0 || _stricmp(name.c_str(), "MPNotes.fng") == 0) return 1;
    if (_stricmp(name.c_str(), "Preferences.fng") == 0 || _stricmp(name.c_str(), "MPPreferences.fng") == 0) return 2;
    if (_stricmp(name.c_str(), "LoadRestart.fng") == 0 || _stricmp(name.c_str(), "MPLoadRestart.fng") == 0) return 3;
    return -1;
}

bool PauseRoot(const std::string& name) {
    return _stricmp(name.c_str(), "Pause.fng") == 0 || _stricmp(name.c_str(), "MPPause.fng") == 0;
}

bool Backdrop(const std::string& name) {
    return PauseRoot(name) || _stricmp(name.c_str(), "Shell.fng") == 0;
}

bool MapPackage(const std::string& package) {
    return _stricmp(package.c_str(), "Map_overlay.fng") == 0 || _stricmp(package.c_str(), "CoopMapOverlay.fng") == 0;
}

Action TextAction(uint32_t hash, const std::string& package) {
    if (MapPackage(package)) {
        for (int level = 1; level <= 11; level++)
            if (hash == feng::Hash("Level" + std::to_string(level) + " text")) return Action::Mission;
    }
    if (Tab(package) >= 0) {
        static const char* headers[] = {"Objectives text", "Notes text", "Preferences text", "Load text"};
        for (int i = 0; i < 4; i++)
            if (hash == feng::Hash(headers[i])) return Action(int(Action::Tab0) + i);
        if (hash == feng::Hash("Leaderboard text")) return Action::Tab0;
        if (hash == feng::Hash("Exit text")) return Action::Tab3;
    }
    if (PauseRoot(package) && (hash == feng::Hash("Press START Text") ||
                              hash == feng::Hash("Press START Text Minus Button Word"))) return Action::Resume;
    if (hash == feng::Hash("X button text")) return Action::Accept;
    if (hash == feng::Hash("Tri button text")) return Action::Back;
    return Action::None;
}

void Objects(uint32_t object, uint32_t package, uint32_t parent_button, bool disabled, int depth, size_t& budget,
             bool shown_only) {
    if (depth > 32) return;
    while (Readable(object, 0x64) && budget) {
        --budget;
        uint32_t flags = R32(object + 0x1C);
        Object info{package, (flags & kButton) ? object : parent_button,
                    R32(object + 0x10), R32(object + 0x0C), disabled || (flags & kDisabled) != 0};
        info.address = object;
        info.button_guid = info.button ? R32(info.button + 0x0C) : 0;
        info.shown_only = shown_only;
        if (!state.objects.emplace(object, info).second) return;
        if (R32(object + 0x18) == 5) Objects(R32(object + 0x60), package, info.button, info.disabled, depth + 1, budget, shown_only);
        object = R32(object + 4);
    }
}

const Object* Drawing() {
    if (!state.rendering || !state.drawing) return nullptr;
    auto it = state.objects.find(state.drawing);
    return it == state.objects.end() || it->second.disabled || it->second.shown_only ? nullptr : &it->second;
}

void Add(const Object& object, const Rect& rect, Action action) {
    if (!rect.Valid() || rect.right - rect.left > 4096 || rect.bottom - rect.top > 4096) return;
    state.frames[object.package].regions.push_back({object, rect, action});
}

// Labels and values are siblings of their button group. Its arrows can be hidden until it gains focus, so the
// label must identify the owning row independently of any arrow geometry.
uint32_t LabelButton(uint32_t hash) {
    static const std::unordered_map<uint32_t, uint32_t> labels = [] {
        std::unordered_map<uint32_t, uint32_t> result;
        for (size_t row = 0; row < menu::kRows; row++) {
            std::string prefix = "PC Row " + std::to_string(row);
            uint32_t button = feng::Hash(prefix + " button");
            result[feng::Hash(prefix + " text")] = button;
            result[feng::Hash(prefix + " value")] = button;
        }
        const char* names[] = {"Crosshairs", "HUD", "Subtitles", "Sound Format", "Music Volume", "SFX Volume",
                               "Turn Speed", "Analog Sensitivity", "Invert Y Axis", "Vibrate", "Grenade Cook"};
        for (const char* name : names) {
            uint32_t button = feng::Hash(std::string(name) + " button");
            for (const char* suffix : {" text", " On", " Off", " Automatic"})
                result[feng::Hash(std::string(name) + suffix)] = button;
        }
        for (const char* value : {"Mono", "Stereo", "Dolby"}) result[feng::Hash(value)] = feng::Hash("Sound Format button");
        // the Custom controller layout: each action's name beside its button's picture, and the sticks' mode
        const char* custom[][2] = {{"Fire_TEXT", "Fire button"}, {"Action_TEXT", "Action button"},
                                   {"ChangeWeapon_TEXT", "Change Weapon button"},
                                   {"GrenadeThrow_TEXT", "Throw Grenade button"}, {"Reload_TEXT", "Reload button"},
                                   {"AimMode_TEXT", "Aim button"}, {"Jump_TEXT", "Jump button"},
                                   {"Crouch_TEXT", "Crouch button"}, {"CenterView_TEXT", "Center View button"},
                                   {"Melee_TEXT", "Melee button"}, {"Analog stick string", "Analog stick button"}};
        for (const auto& [label, button] : custom) result[feng::Hash(label)] = feng::Hash(button);
        return result;
    }();
    auto it = labels.find(hash);
    return it == labels.end() ? 0 : it->second;
}

// an options row: a group with arrows, or one its labels name
bool Row(const Frame& frame, uint32_t button) {
    for (const Region& r : frame.regions)
        if (r.object.button == button && (r.action == Action::Left || r.action == Action::Right)) return true;
    auto it = state.objects.find(button);
    if (it == state.objects.end()) return false;
    for (const auto& [address, object] : state.objects)
        if (object.package == it->second.package && LabelButton(object.hash) == it->second.hash) return true;
    return false;
}

// An options row's arrows can be missing from a frame (they flash on some screens, and a dialog or a script can hide
// them for a moment); a click then fell on the row and stepped it right whichever arrow it was meant for. The
// selected row keeps its arrows' last places for a moment.
struct Arrows {
    Region left, right;
    bool has_left = false, has_right = false;
    int64_t seen = 0;
};
thread_local std::unordered_map<uint32_t, Arrows> g_arrows;  // by button
constexpr int64_t kArrowsKeptMs = 1000;

void KeepArrows(Frame& frame) {
    int64_t now = NowMs();
    std::unordered_map<uint32_t, bool> drawn;
    for (const Region& r : frame.regions) {
        if ((r.action != Action::Left && r.action != Action::Right) || !r.object.button) continue;
        Arrows& a = g_arrows[r.object.button];
        if (a.seen != now) a.has_left = a.has_right = false;
        (r.action == Action::Left ? a.left : a.right) = r;
        (r.action == Action::Left ? a.has_left : a.has_right) = true;
        a.seen = now;
        drawn[r.object.button] = true;
    }
    for (auto it = g_arrows.begin(); it != g_arrows.end();) {
        Arrows& a = it->second;
        if (now - a.seen > kArrowsKeptMs) { it = g_arrows.erase(it); continue; }
        const Region& any = a.has_left ? a.left : a.right;
        auto button = state.objects.find(it->first);
        if (!drawn.contains(it->first) && any.object.package == (frame.regions.empty() ? 0 : frame.regions[0].object.package) &&
            button != state.objects.end() && button->second.guid == any.object.button_guid &&
            R32(any.object.package + 0x80) == it->first) {  // only the selected row shows arrows
            if (a.has_left) frame.regions.push_back(a.left);
            if (a.has_right) frame.regions.push_back(a.right);
        }
        ++it;
    }
}

void Rows(Frame& frame) {
    KeepArrows(frame);
    for (Region& r : frame.regions) {
        if (r.object.button || r.action != Action::None) continue;
        uint32_t button_hash = LabelButton(r.object.hash);
        if (!button_hash) continue;
        for (const auto& [address, object] : state.objects) {
            if (object.package != r.object.package || object.hash != button_hash || object.button != address ||
                object.disabled) continue;
            r.object.button = address;
            r.object.button_guid = object.guid;
            r.action = Action::Focus;
            break;
        }
    }
    std::unordered_map<uint32_t, Region> rows;
    for (const Region& r : frame.regions) {
        if ((r.action != Action::Left && r.action != Action::Right) || !r.object.button) continue;
        Region& row = rows[r.object.button];
        row.object = r.object;
        row.rect.Include(r.rect);
        row.action = Action::Focus;
    }
    for (auto& [button, row] : rows) {
        float centre = (row.rect.top + row.rect.bottom) * 0.5f;
        for (const Region& r : frame.regions) {
            if (r.object.button == button && r.action == Action::Focus) { row.rect.Include(r.rect); continue; }
            if (r.action != Action::None || r.object.button) continue;
            if (std::fabs((r.rect.top + r.rect.bottom) * 0.5f - centre) <= 12) row.rect.Include(r.rect);
        }
        frame.regions.push_back(row);
    }
}

uint32_t ThisCall(X86Context& ctx, uint32_t function, uint32_t receiver, std::initializer_list<uint32_t> args) {
    uint32_t saved = ctx.ecx;
    ctx.ecx = receiver;
    uint32_t result = kernel::CallGuest(ctx, function, args);
    ctx.ecx = saved;
    return result;
}

void Message(X86Context& ctx, const MenuMouseCalls& calls, uint32_t engine, uint32_t package,
             uint32_t message, uint32_t object = 0) {
    ThisCall(ctx, calls.message, engine, {message, 0, package, object ? object : kPackage, state.input_mask});
    ThisCall(ctx, calls.message, engine, {message, object, package, kHandler, state.input_mask});
}

// The controller layout page (the preset picker and the Custom layout under it) reads the controller itself: FEng's
// left, right and accept messages don't reach it, so the mouse presses the dpad and A there, as the controller does.
bool ReadsPad(const std::string& package) {
    return _stricmp(package.c_str(), "ControllerBackground.fng") == 0 ||
           _stricmp(package.c_str(), "CustomController.fng") == 0;
}

// left, right, accept or back to the current package, as its controller would give them
void Press(X86Context& ctx, const MenuMouseCalls& calls, uint32_t engine, uint32_t package, const std::string& name,
           uint32_t message) {
    // the mission map steps its missions on the dpad (the arrows beside the mission's name), as the arrow keys do
    bool pad = ReadsPad(name) || (MapPackage(name) && (message == kLeft || message == kRight));
    if (pad && (message == kLeft || message == kRight || message == kAccept || message == kBack)) {
        if (message == kAccept || message == kBack) input::MenuTap(0, message == kAccept ? 1 : 2);  // A, B
        else input::MenuTap(message == kLeft ? 0x4 : 0x8);                                          // dpad left, right
        return;
    }
    Message(ctx, calls, engine, package, message);
}

void Focus(X86Context& ctx, const MenuMouseCalls& calls, uint32_t engine, uint32_t package, uint32_t object) {
    if (object && object != R32(package + 0x80)) ThisCall(ctx, calls.focus, package, {engine, object, 1});
}

bool Live(const Region& r) {
    if (!Readable(r.object.address, 0x30) || R32(r.object.address + 0x0C) != r.object.guid ||
        R32(r.object.address + 0x10) != r.object.hash) return false;
    uint32_t object = r.object.button;
    if (!object) return true;
    if (!Readable(object, 0x30) || R32(object + 0x0C) != r.object.button_guid ||
        (R32(object + 0x1C) & kDisabled)) return false;
    // The button must still be in this package. A recycled pointer or a newly pushed modal cannot be clicked
    // using a previous frame's geometry.
    uint32_t count = R32(r.object.package + 0x88), list = R32(r.object.package + 0x84);
    if (count > 2048 || !Readable(list, count * 4)) return false;
    for (uint32_t i = 0; i < count; i++) if (R32(list + 4 * i) == object) return true;
    return false;
}

uint32_t Active(uint32_t engine, std::string& name, uint32_t input_mask) {
    // Every package with the player's input gets the pad (sub_00094330 per package), so a menu that opens a dialog over
    // itself can still hold it with its own buttons hidden: the last one that draws something to click wins over one
    // that draws nothing, then the last one with buttons, as before
    uint32_t selected = 0, drawn = 0;
    std::string drawn_name;
    if (!Readable(engine, 0x110) || !G<uint8_t>(engine + 1) || !G<uint8_t>(engine + 2)) return 0;
    for (uint32_t p = R32(engine + 0xE4), n = 0; Readable(p, 0xA4) && n < 128; p = R32(p + 4), n++) {
        if (!(R32(p + 0x1C) & kernel::R16(engine + 0xD8) & input_mask)) continue;
        auto it = state.frames.find(p);
        if (it == state.frames.end() || it->second.name != Name(p)) continue;
        if (Backdrop(it->second.name)) continue;
        bool actionable = std::any_of(it->second.regions.begin(), it->second.regions.end(),
                                      [](const Region& r) { return r.action != Action::None; });
        if (R32(p + 0x88) || Tab(it->second.name) >= 0 || actionable) {
            selected = p;
            name = it->second.name;
        }
        if (actionable) {
            drawn = p;
            drawn_name = it->second.name;
        }
    }
    if (drawn && drawn != selected) {
        name = drawn_name;
        return drawn;
    }
    return selected;
}

constexpr float kArrowMargin = 8.0f;  // guest pixels around an arrow that still take it

const Region* Hit(const Frame& frame, float x, float y) {
    const Region* hit = nullptr;
    int best = -1;
    for (const Region& r : frame.regions) {
        // an arrow is a small picture that pulses: it takes the pointer a little way around it
        bool arrow = r.action == Action::Left || r.action == Action::Right;
        if (r.action == Action::None || !r.rect.Contains(x, y, arrow ? kArrowMargin : 3) || !Live(r)) continue;
        int rank = r.action == Action::Focus ? 0 : r.action == Action::Left || r.action == Action::Right ? 2 : 1;
        if (rank >= best) { hit = &r; best = rank; }
    }
    return hit;
}

// The words of an item are a small target, and the pointer between two items or beside one selected nothing, so the
// focus lagged behind it across a list. Off the words the item takes the pointer anywhere in its row across the list's
// column, up to halfway to its neighbours above and below.
const Region* Nearest(const Frame& frame, float x, float y) {
    constexpr float kColumnMargin = 24.0f, kMaxReach = 24.0f, kLoneReach = 8.0f;
    constexpr size_t kFewChoices = 3;
    constexpr float kChoiceMargin = 96.0f, kChoiceReach = 12.0f;
    struct Band { const Region* region; Rect rect; };
    std::vector<Band> bands;
    for (const Region& r : frame.regions) {
        if ((r.action != Action::Focus && r.action != Action::Activate) || !r.object.button || !Live(r)) continue;
        auto it = std::find_if(bands.begin(), bands.end(),
                               [&](const Band& b) { return b.region->object.button == r.object.button; });
        if (it == bands.end()) bands.push_back({&r, r.rect});
        else it->rect.Include(r.rect);
    }
    const Region* best = nullptr;
    float best_distance = 0;
    for (const Band& b : bands) {
        Rect area = b.rect;
        float up = kLoneReach, down = kLoneReach;
        bool above = false, below = false;
        for (const Band& o : bands) {
            if (&o == &b || o.rect.right <= b.rect.left || o.rect.left >= b.rect.right) continue;
            area.left = std::min(area.left, o.rect.left);
            area.right = std::max(area.right, o.rect.right);
            if (o.rect.bottom <= b.rect.top) {
                float gap = std::min((b.rect.top - o.rect.bottom) * 0.5f, kMaxReach);
                up = above ? std::min(up, gap) : gap;
                above = true;
            } else if (o.rect.top >= b.rect.bottom) {
                float gap = std::min((o.rect.top - b.rect.bottom) * 0.5f, kMaxReach);
                down = below ? std::min(down, gap) : gap;
                below = true;
            }
        }
        // a dialog's few choices (Yes, No): their words are short, so the row reaches well past them
        float margin = bands.size() <= kFewChoices ? kChoiceMargin : kColumnMargin;
        if (bands.size() <= kFewChoices) {
            if (!above) up = kChoiceReach;
            if (!below) down = kChoiceReach;
        }
        if (x < area.left - margin || x >= area.right + margin || y < b.rect.top - up ||
            y >= b.rect.bottom + down) continue;
        float distance = std::fabs(y - (b.rect.top + b.rect.bottom) * 0.5f);
        if (!best || distance < best_distance) { best = b.region; best_distance = distance; }
    }
    return best;
}

// a package's button by its object name, 0 for none
uint32_t ButtonNamed(uint32_t package, const char* name) {
    uint32_t hash = feng::Hash(name);
    for (const auto& [address, object] : state.objects)
        if (object.package == package && object.hash == hash && object.button == address && !object.disabled)
            return address;
    return 0;
}

// The name entry keyboards (passwords, profile and save names: VirtualKeyboard.fng and its like) have a button per
// character, named for it, with BACKSPACE, SEND and "__" for space. A typed key focuses its button and presses it,
// as the dpad and A would, one at a time.
bool NameEntry(uint32_t package) { return ButtonNamed(package, "SEND") && ButtonNamed(package, "BACKSPACE"); }

void Type(X86Context& ctx, const MenuMouseCalls& calls, uint32_t engine, uint32_t package) {
    char c = input::TakeTyped();
    if (!c) return;
    std::string name = c == '\b' ? "BACKSPACE" : c == '\r' ? "SEND" : c == ' ' ? "__" : std::string(1, c);
    uint32_t button = ButtonNamed(package, name.c_str());
    if (!button) return;
    Focus(ctx, calls, engine, package, button);
    input::TypeAccept();  // the keyboard screen types the focused key on A, as the controller does
}

// Development probe: timed pointer clicks through the complete hit-test path, without moving the PC's cursor.
// MOHRS_MENU_MOUSE_PROBE="8:Preferences text,10:Audio button"; times are from the first FEng input update.
void ProbePointer(input::MenuPointer& pointer, const MenuMouseCalls& calls) {
    struct Click { double at; uint32_t hash; };
    static const std::vector<Click> script = [] {
        std::vector<Click> out;
        for (const char* p = std::getenv("MOHRS_MENU_MOUSE_PROBE"); p && *p;) {
            double at;
            char name[64] = {};
            int used = 0;
            if (std::sscanf(p, " %lf:%63[^,]%n", &at, name, &used) != 2) break;
            // "#1796539D" names a region by its hash, as the trace prints it
            out.push_back({at, name[0] == '#' ? uint32_t(std::strtoul(name + 1, nullptr, 16)) : feng::Hash(name)});
            p += used;
            if (*p == ',') p++;
        }
        return out;
    }();
    if (script.empty()) return;
    static const Clock::time_point start = Clock::now();
    double now = std::chrono::duration<double>(Clock::now() - start).count();
    for (const Click& click : script) {
        if (now < click.at - 0.25 || now >= click.at + 0.1) continue;
        for (const auto& [package, frame] : state.frames) for (const Region& r : frame.regions) {
            if (r.object.hash != click.hash || !r.rect.Valid()) continue;
            app::Mode output = app::Output();
            pointer.width = float(output.width); pointer.height = float(output.height);
            float guest_w = float(R32(calls.width)), guest_h = float(R32(calls.height));
            float x = (r.rect.left + r.rect.right) * 0.5f, y = (r.rect.top + r.rect.bottom) * 0.5f;
            bool wide = app::Widescreen();
            float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
            float w = std::min(pointer.width, pointer.height * aspect), h = w / aspect;
            pointer.x = (pointer.width - w) * 0.5f + x * w / guest_w;
            // A distinct pre-click move lets hover run after the probe's foreground/size transition settles.
            if (now < click.at - 0.1) pointer.x -= 1;
            pointer.y = (pointer.height - h) * 0.5f + y * h / guest_h;
            pointer.active = true;
            pointer.buttons = now >= click.at ? 1u : 0u;
            pointer.pressed = 0; pointer.wheel = 0;
            return;
        }
    }
}

void Update(X86Context& ctx, uint32_t engine, const MenuMouseCalls& calls) {
    std::string name;
    uint32_t input_mask = KeyboardMask(calls);
    uint32_t package = state.engine == engine ? Active(engine, name, input_mask) : 0;
    bool fresh = Clock::now() - state.rendered < std::chrono::milliseconds(500);
    if (!fresh) package = 0;
    input::SetMenuInput(package != 0);
    if (!package) cursor_rendered_ms = 0;
    static thread_local bool announced = false;
    if (package && !announced) {
        announced = true;
        LOGR("menu mouse: enabled; hover, left click, right click/back and wheel");
    }
    input::MenuPointer pointer = input::ReadMenuPointer();
    ProbePointer(pointer, calls);
    bool changed = input_mask != state.input_mask || package != state.active || name != state.active_name ||
                   pointer.active != state.foreground || pointer.width != state.width || pointer.height != state.height;
    uint32_t pressed = state.buttons.Advance(pointer.buttons, pointer.pressed, changed || !pointer.active || !package);
    bool moved = pointer.x != state.x || pointer.y != state.y;
    state.active = package; state.active_name = name; state.foreground = pointer.active;
    state.input_mask = input_mask;
    state.x = pointer.x; state.y = pointer.y; state.width = pointer.width; state.height = pointer.height;
    if (changed) {
        state.press_object = state.press_package = 0;
        if (Trace()) LOGR("menu mouse: active %08X %s, %zu regions", package, name.c_str(),
                          package ? state.frames[package].regions.size() : 0);
        if (Trace() && package) for (const Region& r : state.frames[package].regions) {
            if (r.action != Action::None) LOGR("menu mouse: region hash %08X button %08X action %d rect %.1f %.1f %.1f %.1f",
                r.object.hash, r.object.button, int(r.action), r.rect.left, r.rect.top, r.rect.right, r.rect.bottom);
        }
    }
    bool typing = package && NameEntry(package);
    input::SetTextEntry(typing);
    if (typing && !changed) Type(ctx, calls, engine, package);
    if (!package || !pointer.active) return;
    int tab = Tab(name);
    if (!(pointer.buttons & 1) && state.press_object) {
        if (state.press_package == package && Readable(state.press_object, 0x30) &&
            R32(state.press_object + 0x0C) == state.press_guid &&
            ThisCall(ctx, calls.object_message, state.press_object, {kReleased})) {
            Message(ctx, calls, engine, package, kReleased, state.press_object);
        }
        state.press_object = 0;
    }
    float x, y;
    if (!menu::MapPointer(pointer.x, pointer.y, pointer.width, pointer.height, float(R32(calls.width)),
                          float(R32(calls.height)), app::Widescreen(), x, y)) return;
    if (changed) return;
    const Region* hit = Hit(state.frames[package], x, y);
    // Select/back hints live in the backdrop and act on the CURRENT package, including PC Options' confirm/revert.
    // Resume is only available on the pause pages, never through an options screen or a confirmation dialog.
    if (!hit) {
        for (const auto& [p, frame] : state.frames)
            if (Backdrop(frame.name) || frame.shown_only) {
                bool present = false;
                for (uint32_t live = R32(engine + 0xE4), n = 0; Readable(live, 0xA4) && n < 128; live = R32(live + 4), n++)
                    if (live == p && Name(live) == frame.name) { present = true; break; }
                if (!present || (!frame.shown_only && !(R32(p + 0x1C) & input_mask))) continue;
                const Region* root_hit = Hit(frame, x, y);
                if (root_hit && ((root_hit->action == Action::Resume && tab >= 0) ||
                                 root_hit->action == Action::Accept || root_hit->action == Action::Back)) hit = root_hit;
            }
    }
    if (!hit) hit = Nearest(state.frames[package], x, y);
    if (!moved && !pressed && !pointer.wheel) return;
    input::UseMenuMouse();
    if (pressed & 2) {
        if (tab >= 0) input::MenuResume();
        else Press(ctx, calls, engine, package, name, kBack);
        return;
    }
    if (!hit) return;
    uint32_t button = hit->object.button;
    // the map's arrows step the selected mission: they never take the focus from it
    if (MapPackage(name) && (hit->action == Action::Left || hit->action == Action::Right)) button = 0;
    if (hit->action == Action::Mission) {
        // The bottom title is the selected mission's display, not an independent selectable button. Use the
        // native current selection, including its unlock/input checks, just as the controller's Confirm does.
        button = R32(package + 0x80);
        auto it = state.objects.find(button);
        if (it == state.objects.end() || it->second.package != package || it->second.button != button ||
            it->second.disabled) return;
        Region selected = *hit;
        selected.object.button = button;
        selected.object.button_guid = it->second.guid;
        if (!Live(selected)) return;
    }
    bool was_focused = button && R32(package + 0x80) == button;
    Focus(ctx, calls, engine, package, button);
    if (Trace() && (pressed || pointer.wheel)) LOGR("menu mouse: %s button %08X action %d at %.1f %.1f",
                                                   name.c_str(), button, int(hit->action), x, y);
    if (pointer.wheel && button) {
        bool row = hit->action == Action::Focus || hit->action == Action::Left || hit->action == Action::Right;
        if (row) Press(ctx, calls, engine, package, name, pointer.wheel > 0 ? kRight : kLeft);
        else {
            uint32_t next = ThisCall(ctx, calls.navigate, package + 0x84,
                                    {button, pointer.wheel > 0 ? 0u : 4u, R32(engine + 0x10C), 0});
            Focus(ctx, calls, engine, package, next);
        }
    }
    if (!(pressed & 1)) return;
    switch (hit->action) {
        case Action::Focus:
            // A group with its own press response is an ordinary compound button, rather than an options row. A group
            // that is no row (a save slot: the package answers the pad's accept for its current button) takes a click
            // on it once selected as accept.
            if (!ThisCall(ctx, calls.object_message, button, {kPressed})) {
                // an options row only takes the focus: its arrows step it
                if (was_focused && !Row(state.frames[package], button)) Press(ctx, calls, engine, package, name, kAccept);
                break;
            }
            [[fallthrough]];
        case Action::Mission:
        case Action::Activate:
            // a mission on the map: the first click picks it, a click on the picked one goes on, as A does
            if (hit->action == Action::Activate && was_focused && MapPackage(name)) {
                Message(ctx, calls, engine, package, kAccept);
                break;
            }
            if (ThisCall(ctx, calls.object_message, button, {kPressed})) {
                Message(ctx, calls, engine, package, kPressed, button);
                state.press_object = button; state.press_package = package;
                state.press_guid = R32(button + 0x0C);
            } else Press(ctx, calls, engine, package, name, kAccept);
            break;
        case Action::Left: Press(ctx, calls, engine, package, name, kLeft); break;
        case Action::Right: Press(ctx, calls, engine, package, name, kRight); break;
        case Action::Accept: Press(ctx, calls, engine, package, name, kAccept); break;
        case Action::Back: Press(ctx, calls, engine, package, name, kBack); break;
        case Action::Resume: input::MenuResume(); break;
        case Action::Tab0: case Action::Tab1: case Action::Tab2: case Action::Tab3: {
            int target = int(hit->action) - int(Action::Tab0);
            if (tab < 0 || target == tab) break;
            state.tab_package = package;
            state.tab_name = name;
            state.tab_message = feng::Hash(menu::kPauseTabEvents[target]);
            break;
        }
        default: break;
    }
}

// The bottom bar's controller prompts (the A, B and dpad pictures, named for the PlayStation: X_button, Tri_button,
// DPad_blank) give way to the keys while the keyboard and mouse are in use, and come back with the controller, frame by
// frame as the menu draws: the picture is not drawn and its words name the key ("ENTER CONFIRM"), starting where the
// picture was. Only a picture with its words beside it is a prompt; the controller layout pages keep theirs. On a name
// entry keyboard the keys type, so its words say what Enter and Escape do there.
struct PromptKind {
    const char* image;
    const char* words;        // the words' object, in a group with the picture
    const char* entry_words;  // the words' object on a name entry keyboard
    const char* key;
    const char* entry;        // what a name entry keyboard shows, or null for the key and the words
};
constexpr PromptKind kPromptKinds[] = {{"X_button", "X button text", "FEFont12(43)", "ENTER", "ENTER DONE"},
                                       {"Tri_button", "Tri button text", "FEFont12(44)", "BACKSPACE", "ESC BACK"},
                                       {"DPad_blank", "DPad text", "FEFont12", "ARROWS", nullptr}};
constexpr int kPromptCount = int(std::size(kPromptKinds));

struct Bar {
    bool entry = false;
    bool has[kPromptCount] = {};
};
thread_local std::unordered_map<uint32_t, Bar> g_bars;                 // this frame's, by package
thread_local std::unordered_map<uint64_t, float> g_picture_left;      // a prompt's picture, last drawn: package, kind
thread_local std::vector<std::pair<std::string, uint32_t>> g_shown;   // the words shown, in guest memory

bool HasObject(uint32_t package, uint32_t hash) {
    for (const auto& [address, object] : state.objects)
        if (object.package == package && object.hash == hash) return true;
    return false;
}

const Bar& BarOf(uint32_t package) {
    auto [it, made] = g_bars.try_emplace(package);
    if (made) {
        Bar& bar = it->second;
        bar.entry = NameEntry(package);
        for (int i = 0; i < kPromptCount; i++) {
            const PromptKind& k = kPromptKinds[i];
            bar.has[i] = HasObject(package, feng::Hash(k.image)) &&
                         HasObject(package, feng::Hash(bar.entry ? k.entry_words : k.words));
        }
    }
    return it->second;
}

// the prompt whose picture (or words) the object is, -1 for none
int PromptOf(const Object& object, bool words) {
    if (!input::KeyboardActive()) return -1;
    const Bar& bar = BarOf(object.package);
    for (int i = 0; i < kPromptCount; i++) {
        const PromptKind& k = kPromptKinds[i];
        const char* name = words ? (bar.entry ? k.entry_words : k.words) : k.image;
        if (bar.has[i] && object.hash == feng::Hash(name)) return i;
    }
    return -1;
}

uint64_t PictureKey(uint32_t package, int kind) { return uint64_t(package) << 8 | uint32_t(kind); }

uint32_t Shown(const std::string& text) {
    for (const auto& [s, va] : g_shown)
        if (s == text) return va;
    uint32_t va = mem::AllocSystem(uint32_t(text.size() + 1), 4);
    if (!va) return 0;
    std::memcpy(kernel::HostPtr(va), text.c_str(), text.size() + 1);
    g_shown.emplace_back(text, va);
    return va;
}

float MeasureLeft(X86Context& ctx, const MenuMouseCalls& calls, uint32_t font, uint32_t text) {
    X86Context saved = ctx;
    ctx.esp -= 32;
    uint32_t out = ctx.esp;
    for (int i = 0; i < 4; i++) kernel::W32(out + 4 * i, 0);
    kernel::CallGuest(ctx, calls.measure_text, {font, text, out, out + 4, out + 8, out + 12});
    float left = G<float>(out);
    ctx = saved;
    return left;
}

}  // namespace

bool MenuMouseCursorVisible() {
    int64_t rendered = cursor_rendered_ms.load();
    return rendered != 0 && NowMs() - rendered < kCursorFreshMs;
}

void MenuMouseUpdate(X86Context& ctx, uint8_t* base, GuestFunction original, const MenuMouseCalls& calls) {
    // Helpers can use x87/SIMD and flags too; the original update must receive its complete incoming context.
    X86Context saved = ctx;
    Update(ctx, ctx.ecx, calls);
    ctx = saved;
    original(ctx, base);
    // Queue direct transitions after this update's initialization events have dispatched. This preserves their
    // ordering without waiting for animations or opening any intervening tabs. The cloned package response
    // retains the native handler/overlay cleanup and replaces the page in the normal FEng transition queue.
    if (state.tab_package) {
        X86Context result = ctx;
        uint32_t engine = saved.ecx;
        if (Readable(engine, 0x110) && G<uint8_t>(engine + 1) && G<uint8_t>(engine + 2) &&
            KeyboardMask(calls) == state.input_mask) {
            for (uint32_t p = R32(engine + 0xE4), n = 0; Readable(p, 0xA4) && n < 128; p = R32(p + 4), n++) {
                if (p != state.tab_package || Name(p) != state.tab_name ||
                    !(R32(p + 0x1C) & kernel::R16(engine + 0xD8) & state.input_mask)) continue;
                Message(ctx, calls, engine, p, state.tab_message);
                if (Trace()) LOGR("menu mouse: direct tab from %s, message %08X", state.tab_name.c_str(), state.tab_message);
                break;
            }
        }
        state.tab_package = 0;
        ctx = result;
    }
}

void MenuMouseRender(X86Context& ctx, uint8_t* base, GuestFunction original, const MenuMouseCalls& calls) {
    state.engine = ctx.ecx;
    state.objects.clear(); state.frames.clear(); state.drawing = 0;
    state.object_draws = state.known_draws = state.text_draws = state.sprite_draws = 0;
    g_bars.clear();
    uint32_t callback = R32(R32(R32(ctx.ecx + 0x10C)) + 0x38);
    uint32_t input_mask = KeyboardMask(calls);
    auto scan_start = std::chrono::steady_clock::now();
    // in play no package takes the player's input (the HUD): nothing is scanned then, the menus' overlays only
    // matter beside a menu
    bool menu_up = false;
    for (uint32_t p = R32(ctx.ecx + 0xE4), n = 0; Readable(p, 0xA4) && n < 128; p = R32(p + 4), n++)
        if (R32(p + 0x1C) & input_mask) { menu_up = true; break; }
    for (uint32_t p = R32(ctx.ecx + 0xE4), n = 0; menu_up && Readable(p, 0xA4) && n < 128; p = R32(p + 4), n++) {
        std::string name = Name(p);
        static thread_local int option_reports = 0;
        if (Trace() && name == menu::kScreen && option_reports < 6) {
            option_reports++;
            LOGR("menu mouse: options input flags %02X %02X ports %X mask %X buttons %u focus %08X",
                 G<uint8_t>(ctx.ecx + 1), G<uint8_t>(ctx.ecx + 2), kernel::R16(ctx.ecx + 0xD8),
                 R32(p + 0x1C), R32(p + 0x88), R32(p + 0x80));
            uint32_t parent = R32(p + 0x30);
            LOGR("menu mouse: options package %08X parent %08X %s", p, parent,
                 Readable(parent, 0xA4) ? Name(parent).c_str() : "none");
        }
        // A package without this player's input is still scanned, so its prompts follow the device too (the save
        // screen's bar is an overlay of its own), but none of its objects can be clicked: only its select and back
        // words, which act on the menu as the backdrop's do.
        bool shown_only = !(R32(p + 0x1C) & input_mask) && !Backdrop(name);
        Frame& frame = state.frames[p];
        frame.name = name;
        frame.shown_only = shown_only;
        size_t budget = shown_only ? 1024 : 4096;
        Objects(R32(p + 0x40), p, 0, false, 0, budget, shown_only);
    }
    auto scan_end = std::chrono::steady_clock::now();
    state.rendering = true;
    original(ctx, base);
    state.rendering = false;
    auto rows_start = std::chrono::steady_clock::now();
    for (auto& [p, frame] : state.frames) Rows(frame);
    if (diag::Perf()) {
        // perf: what the menu mouse costs a frame, once a second
        static thread_local double spent_ms = 0;
        static thread_local uint64_t frames = 0, objects = 0;
        static thread_local auto since = scan_start;
        auto now = std::chrono::steady_clock::now();
        spent_ms += std::chrono::duration<double, std::milli>(scan_end - scan_start + (now - rows_start)).count();
        frames++;
        objects += state.objects.size();
        if (now - since >= std::chrono::seconds(1)) {
            LOGR("perf: menu mouse %.3f ms/frame, %llu objects/frame%s", spent_ms / double(frames),
                 (unsigned long long)(objects / frames), menu_up ? "" : " (in play)");
            spent_ms = 0;
            frames = objects = 0;
            since = now;
        }
    }
    if (Trace()) {
        // the packages drawn, when they change
        static thread_local std::string last;
        std::string now;
        for (uint32_t p = R32(state.engine + 0xE4), n = 0; Readable(p, 0xA4) && n < 128; p = R32(p + 4), n++) {
            auto it = state.frames.find(p);
            char line[160];
            std::snprintf(line, sizeof line, " %s(mask %X buttons %u%s regions %zu)", Name(p).c_str(), R32(p + 0x1C),
                          R32(p + 0x88), it != state.frames.end() && it->second.shown_only ? " shown" : "",
                          it == state.frames.end() ? size_t(0) : it->second.regions.size());
            now += line;
        }
        if (now != last) {
            last = now;
            for (size_t at = 0; at < now.size(); at += 200) LOGR("menu mouse: packages %s", now.substr(at, 200).c_str());
            for (const auto& [address, object] : state.objects)
                if (object.button == address)
                    LOGR("menu mouse: button %08X hash %08X in %s type %u flags %08X%s", address, object.hash,
                         Name(object.package).c_str(), R32(address + 0x18), R32(address + 0x1C),
                         object.disabled ? " disabled" : "");
        }
    }
    static thread_local int reports = 0;
    if (Trace() && !state.objects.empty() && reports < 8) {
        reports++;
        LOGR("menu mouse: draw callback %08X objects %zu calls %u known %u text %u sprite %u", callback,
             state.objects.size(), state.object_draws, state.known_draws, state.text_draws, state.sprite_draws);
    }
    state.rendered = Clock::now();
    // Publish only completed draws of real controls. Loading/movie frames don't keep a previous menu's cursor
    // alive, even if FEng stops updating and its last input-ownership flag remains set.
    std::string active_name;
    uint32_t active = Active(state.engine, active_name, input_mask);
    bool interactive = active && std::any_of(state.frames[active].regions.begin(), state.frames[active].regions.end(),
                                             [](const Region& r) { return r.action != Action::None; });
    cursor_rendered_ms = interactive ? NowMs() : 0;
}

void MenuMouseObject(X86Context& ctx, uint8_t* base, GuestFunction original) {
    uint32_t previous = state.drawing;
    state.drawing = R32(ctx.esp + 4);
    if (state.rendering) {
        state.object_draws++;
        if (state.objects.contains(state.drawing)) state.known_draws++;
    }
    original(ctx, base);
    state.drawing = previous;
}

void MenuMouseText(X86Context& ctx, uint8_t* base, GuestFunction original, const MenuMouseCalls& calls) {
    // a prompt's words for the keys, put back once drawn: the caller's arguments
    uint32_t was_text = R32(ctx.esp + 16), was_x = R32(ctx.esp + 8);
    bool swapped = false;
    if (state.rendering && state.drawing) {
        auto it = state.objects.find(state.drawing);
        int kind = it == state.objects.end() ? -1 : PromptOf(it->second, true);
        if (kind >= 0 && Readable(was_text, 1)) {
            const PromptKind& k = kPromptKinds[kind];
            std::string words = kernel::ReadCString(was_text, 63);
            std::string shown = BarOf(it->second.package).entry && k.entry ? k.entry : std::string(k.key) + " " + words;
            if (uint32_t va = Shown(shown)) {
                kernel::W32(ctx.esp + 16, va);
                swapped = true;
                auto left = g_picture_left.find(PictureKey(it->second.package, kind));
                if (left != g_picture_left.end()) {
                    float x = left->second - MeasureLeft(ctx, calls, R32(ctx.esp + 4), va);
                    std::memcpy(kernel::HostPtr(ctx.esp + 8), &x, 4);
                }
            }
        }
    }
    if (const Object* object = Drawing()) {
        state.text_draws++;
        uint32_t font = R32(ctx.esp + 4), text = R32(ctx.esp + 16);
        float x = G<float>(ctx.esp + 8), y = G<float>(ctx.esp + 12);
        X86Context saved = ctx;
        ctx.esp -= 32;
        uint32_t out = ctx.esp;
        for (int i = 0; i < 4; i++) kernel::W32(out + 4 * i, 0);
        kernel::CallGuest(ctx, calls.measure_text, {font, text, out, out + 4, out + 8, out + 12});
        float left = x + G<float>(out), top = y + G<float>(out + 4);
        Rect rect{left, top, left + G<float>(out + 8), top + G<float>(out + 12)};
        ctx = saved;
        Action action = object->button ? (R32(object->button + 0x18) == 5 ? Action::Focus : Action::Activate) :
                        TextAction(object->hash, state.frames[object->package].name);
        Add(*object, rect, action);
    } else if (state.rendering && state.drawing) {
        // an overlay's select and back words
        auto it = state.objects.find(state.drawing);
        Action action = it == state.objects.end() || !it->second.shown_only || it->second.disabled ? Action::None :
                        TextAction(it->second.hash, state.frames[it->second.package].name);
        if (action == Action::Accept || action == Action::Back) {
            uint32_t font = R32(ctx.esp + 4), text = R32(ctx.esp + 16);
            float x = G<float>(ctx.esp + 8), y = G<float>(ctx.esp + 12);
            X86Context saved = ctx;
            ctx.esp -= 32;
            uint32_t out = ctx.esp;
            for (int i = 0; i < 4; i++) kernel::W32(out + 4 * i, 0);
            kernel::CallGuest(ctx, calls.measure_text, {font, text, out, out + 4, out + 8, out + 12});
            float left = x + G<float>(out), top = y + G<float>(out + 4);
            Rect rect{left, top, left + G<float>(out + 8), top + G<float>(out + 12)};
            ctx = saved;
            Add(it->second, rect, action);
        }
    }
    uint32_t esp = ctx.esp;
    original(ctx, base);
    if (swapped) {
        kernel::W32(esp + 16, was_text);
        kernel::W32(esp + 8, was_x);
    }
}

// a sprite's corners on the screen, as drawn
Rect SpriteRect(X86Context& ctx, const MenuMouseCalls& calls) {
    uint32_t points = R32(ctx.esp + 4), sprite = ctx.ecx;
    Rect rect;
    if (!Readable(points, 64)) return rect;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (int i = 0; i < 4; i++) {
        float x = G<float>(points + 16 * i), y = G<float>(points + 16 * i + 4);
        if (G<uint8_t>(sprite + 0x1A)) x = std::floor(G<float>(calls.scale_x) * x + G<float>(calls.offset_x) + 0.5f);
        if (G<uint8_t>(sprite + 0x19)) y = std::floor(G<float>(calls.scale_y) * y + G<float>(calls.offset_y) + 0.5f);
        if (!i) min_x = max_x = x, min_y = max_y = y;
        min_x = std::min(min_x, x); min_y = std::min(min_y, y);
        max_x = std::max(max_x, x); max_y = std::max(max_y, y);
    }
    return {min_x, min_y, max_x, max_y};
}

void MenuMouseSprite(X86Context& ctx, uint8_t* base, GuestFunction original, const MenuMouseCalls& calls) {
    if (state.rendering && state.drawing) {
        auto it = state.objects.find(state.drawing);
        int kind = it == state.objects.end() ? -1 : PromptOf(it->second, false);
        if (kind >= 0) {
            Rect rect = SpriteRect(ctx, calls);
            if (rect.Valid()) g_picture_left[PictureKey(it->second.package, kind)] = rect.left;
            ctx.esp += 4 + 12;  // not drawn: the thiscall's ret 0xC
            return;
        }
    }
    if (const Object* object = Drawing(); object && object->button) {
        state.sprite_draws++;
        uint32_t points = R32(ctx.esp + 4), sprite = ctx.ecx;
        Rect rect;
        if (Readable(points, 64)) {
            float min_x = G<float>(points), min_y = G<float>(points + 4), max_x = min_x, max_y = min_y;
            for (int i = 0; i < 4; i++) {
                float x = G<float>(points + 16 * i), y = G<float>(points + 16 * i + 4);
                if (G<uint8_t>(sprite + 0x1A)) x = std::floor(G<float>(calls.scale_x) * x + G<float>(calls.offset_x) + 0.5f);
                if (G<uint8_t>(sprite + 0x19)) y = std::floor(G<float>(calls.scale_y) * y + G<float>(calls.offset_y) + 0.5f);
                if (!i) min_x = max_x = x, min_y = max_y = y;
                min_x = std::min(min_x, x); min_y = std::min(min_y, y);
                max_x = std::max(max_x, x); max_y = std::max(max_y, y);
            }
            rect = {min_x, min_y, max_x, max_y};
        }
        Action action = R32(object->button + 0x18) == 5 ? Action::Focus : Action::Activate;
        if (object->hash == feng::Hash("arrow_left")) action = Action::Left;
        if (object->hash == feng::Hash("arrow_right")) action = Action::Right;
        Add(*object, rect, action);
    } else if (const Object* arrows = Drawing()) {
        // a page's own arrows, beside no button: the controller layout's preset picker steps with them
        bool left = arrows->hash == feng::Hash("arrow_left"), right = arrows->hash == feng::Hash("arrow_right");
        if (left || right) Add(*arrows, SpriteRect(ctx, calls), left ? Action::Left : Action::Right);
    }
    original(ctx, base);
}

}  // namespace game
