// Keyboard and mouse prompts: the in-game hints name the controller's actions ("Press ACTION to Open Door", "Press
// START button to View Objectives"). While the keyboard is in use they name the bound keys instead ("Press F to Open
// Door"), and the controller's words come back as soon as a controller is used again.
//
// The hints are a string table (Global@<language>@prompts.str, its .off a u16 count then each string's length).
// sub_0011FF10 builds the table's entries from it: the table object's + 0 points at 8 byte entries {text pointer,
// u16 length, flag byte}, + 0x10 holds the count, and the game's lookup (sub_0011E830, by name hash) hands out the
// entry's pointer each time it is asked. So the hints are swapped by pointing an entry at a rewritten copy, kept in
// system memory, and back at the file's own text.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using kernel::R32;
using kernel::W32;

namespace {

constexpr uint32_t kEntries = 0x0;  // table object: the entry array
constexpr uint32_t kCount = 0x10;   // table object: the entry count
constexpr uint32_t kEntrySize = 8;  // entry: text pointer, u16 length with the terminator, flag byte
constexpr size_t kMaxTables = 8;

uint16_t R16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, kernel::HostPtr(va), 2);
    return v;
}
void W16(uint32_t va, uint16_t v) { std::memcpy(kernel::HostPtr(va), &v, 2); }

// the display name of a bound key: the mouse buttons spelt out, the rest as the settings name them
std::string Key(const char* id) {
    std::string key = input::KeyName(id);
    if (key == "LMB") return "Left Mouse";
    if (key == "RMB") return "Right Mouse";
    if (key == "MMB") return "Middle Mouse";
    return key;
}

struct Rule {
    std::regex pattern;
    std::vector<const char*> keys;  // the keys' names joined by '/' take the words after the verb captured as $1
};

// the controller words of the hints. Only the words after press, tap, hold or use, so the controls screen's own
// labels (ACTION, JUMP ...) stay as they are
const std::vector<Rule>& Rules() {
    static const std::vector<Rule> rules = [] {
        using std::regex_constants::ECMAScript;
        using std::regex_constants::icase;
        const auto r = [](const std::string& p) { return std::regex(p, ECMAScript | icase); };
        std::vector<Rule> out;
        // "press the `ACTION' button", "Press and hold the 'ACTION' button", "Press ACTION", "press action"
        for (const char* word : {"action", "fire", "jump", "crouch"}) {
            std::string w = word;
            out.push_back({r("\\b(press(?: and hold)?) the [`']?" + w + "'? button"), {word}});
            out.push_back({r("\\b(press(?: and hold)?) [`']?" + w + "\\b'?"), {word}});
        }
        out.push_back({r("\\b(press) start(?: button)?\\b"), {"pause"}});
        out.push_back({r("\\b(press) (?:back|select button|\\+control pad right|call hq)\\b"), {"back"}});
        out.push_back({r("\\b(press) the information button\\b"), {"back"}});
        out.push_back({r("\\b(tap|hold) grenade button\\b"), {"grenade_throw"}});
        out.push_back({r("\\b(use) aim\\b"), {"aim_mode"}});
        out.push_back({r("\\b(use) directional button\\b"), {"sniper_zoom_up", "sniper_zoom_down"}});
        out.push_back({r("\\b(press) x\\b"), {"action"}});  // the valves, which open with Action
        return out;
    }();
    return rules;
}

// the controller missing messages ask for its START button, not a key
bool NamesButtons(const std::string& text) {
    if (text.find("controller") != std::string::npos) return false;
    for (const Rule& rule : Rules())
        if (std::regex_search(text, rule.pattern)) return true;
    return false;
}

// a hint's controller words for the keys' names; a word whose key is unbound stays
std::string ForKeys(const std::string& text) {
    std::string s = text;
    for (const Rule& rule : Rules()) {
        std::string keys;
        for (const char* id : rule.keys) {
            std::string key = Key(id);
            if (key.empty()) {
                keys.clear();
                break;
            }
            if (!keys.empty()) keys += '/';
            keys += key;
        }
        if (!keys.empty()) s = std::regex_replace(s, rule.pattern, "$1 " + keys);
    }
    return s;
}

// a rewritten hint in system memory, one copy per distinct text so a table built again reuses them
uint32_t Copy(const std::string& s) {
    static std::vector<std::pair<std::string, uint32_t>> kept;
    for (const auto& [text, va] : kept)
        if (text == s) return va;
    uint32_t va = mem::AllocSystem(uint32_t(s.size() + 1), 4);
    if (!va) return 0;
    std::memcpy(kernel::HostPtr(va), s.c_str(), s.size() + 1);
    kept.emplace_back(s, va);
    return va;
}

struct Hint {
    uint32_t index;
    uint32_t text;     // the file's own text
    uint16_t length;   // its length with the terminator
    std::string words;  // the text, read once
    uint32_t keys = 0;  // the rewritten copy the entry points at now, 0 for the file's text
};
struct Table {
    uint32_t object = 0, entries = 0;
    std::vector<Hint> hints;
};
std::vector<Table> g_tables;
bool g_keys = false;          // the tables name the keys
uint32_t g_version = ~0u;     // the bindings they were named from

// still the table that was built: a freed table's memory taken for something else fails this
bool Live(const Table& t) {
    if (R32(t.object + kEntries) != t.entries) return false;
    for (const Hint& h : t.hints) {
        uint32_t text = R32(t.entries + h.index * kEntrySize);
        if (text != h.text && text != h.keys) return false;
    }
    return true;
}

void Apply(Table& t, bool keys) {
    size_t named = 0;
    for (Hint& h : t.hints) {
        uint32_t entry = t.entries + h.index * kEntrySize;
        uint32_t text = 0;
        if (keys) {
            std::string named = ForKeys(h.words);
            if (named != h.words) text = Copy(named);
        }
        h.keys = text;
        W32(entry, text ? text : h.text);
        W16(entry + 4, text ? uint16_t(std::strlen(static_cast<const char*>(kernel::HostPtr(text))) + 1) : h.length);
        named += text != 0;
    }
    // what the game's lookup now hands out for the first hint
    const char* first =
        t.hints.empty() ? "" : static_cast<const char*>(kernel::HostPtr(R32(t.entries + t.hints[0].index * kEntrySize)));
    LOGI("prompts: %s, %u hints name keys (\"%s\")", keys ? "keyboard" : "controller", unsigned(named), first);
}

}  // namespace

// game thread, every input update: the hints follow the last device used and the bindings
void RefreshPrompts() {
    bool keys = input::KeyboardActive();
    uint32_t version = input::BindingsVersion();
    if (keys == g_keys && (!keys || version == g_version)) return;
    g_keys = keys;
    g_version = version;
    for (size_t i = 0; i < g_tables.size();) {
        if (!Live(g_tables[i])) {
            g_tables.erase(g_tables.begin() + ptrdiff_t(i));
            continue;
        }
        Apply(g_tables[i], keys);
        i++;
    }
}

// a string table's entries built from its file: its hints noted, and named for the keys when they are in use
X86_EXTERN_FUNC(__imp__sub_0011FF10);
X86_FUNC(sub_0011FF10) {
    uint32_t object = ctx.ecx;
    __imp__sub_0011FF10(ctx, base);
    Table t;
    t.object = object;
    t.entries = R32(object + kEntries);
    uint32_t count = R32(object + kCount);
    if (!t.entries || count > 0x10000) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = t.entries + i * kEntrySize;
        uint32_t text = R32(entry);
        if (!text) continue;
        uint16_t length = R16(entry + 4);
        std::string words(static_cast<const char*>(kernel::HostPtr(text)), length ? length - 1u : 0u);
        words.resize(std::strlen(words.c_str()));
        if (!NamesButtons(words)) continue;
        t.hints.push_back({i, text, length, std::move(words)});
    }
    for (auto it = g_tables.begin(); it != g_tables.end();)
        it = it->object == object || !Live(*it) ? g_tables.erase(it) : it + 1;
    if (t.hints.empty()) return;
    LOGI("prompts: %u of %u strings name controller buttons", unsigned(t.hints.size()), count);
    if (g_keys) Apply(t, true);
    if (g_tables.size() < kMaxTables) g_tables.push_back(std::move(t));
}
