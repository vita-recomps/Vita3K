// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "settings_menu.h"

#include "input.h"

#include <overlay/controls.h>
#include <overlay/font.h>
#include <util/string_utils.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace game_frontend {

namespace {

using overlay::color4f;
using overlay::label;
using overlay::overlay_element;
using overlay::pad_button;
using overlay::rounded_rect;
using Kind = SettingSpec::Kind;

// the 960x544 virtual screen
constexpr int16_t PANEL_X = 110, PANEL_Y = 30, PANEL_W = 740, PANEL_H = 484;
constexpr int16_t TABS_Y = PANEL_Y + 52;
constexpr int16_t ROWS_Y = PANEL_Y + 92, ROW_H = 34;
constexpr size_t VISIBLE_ROWS = 8;
constexpr int16_t HELP_Y = ROWS_Y + ROW_H * static_cast<int16_t>(VISIBLE_ROWS) + 6;
constexpr int16_t FOOTER_Y = PANEL_Y + PANEL_H - 28;

constexpr color4f TEXT{ 0.92f, 0.92f, 0.92f, 1.f };
constexpr color4f TEXT_DIM{ 0.62f, 0.62f, 0.64f, 1.f };
constexpr color4f ACCENT{ 0.80f, 0.16f, 0.16f, 1.f };
constexpr color4f SELECTED{ 0.55f, 0.10f, 0.10f, 0.85f };
constexpr color4f PANEL{ 0.07f, 0.07f, 0.08f, 0.94f };
constexpr color4f CLEAR{ 0.f, 0.f, 0.f, 0.f };

// the Controls page's columns: the key, the second key, the gamepad button, the second gamepad button
constexpr int16_t CELL_X = PANEL_X + 250, CELL_W = 112, CELL_GAP = 6;
constexpr size_t PAD_COLUMN = 2, COLUMNS = 4;
// Android shows the gamepad's columns only (no keyboard there to speak of)
#ifdef __ANDROID__
constexpr size_t FIRST_COLUMN = 2;
#else
constexpr size_t FIRST_COLUMN = 0;
#endif
// the key that stops waiting for a gamepad button (Android's Back arrives as a key)
#ifdef __ANDROID__
#define CANCEL_KEY "Back"
#else
#define CANCEL_KEY "Esc"
#endif
constexpr int16_t cell_x(size_t column) { return static_cast<int16_t>(CELL_X + (CELL_W + CELL_GAP) * static_cast<int16_t>(column)); }
// a binding waits this long for its input, ignores input for the first moments (the press that started
// it), and the menu's own buttons are ignored for a moment after (the bound press reaching the menu)
constexpr auto LISTEN_TIMEOUT = std::chrono::seconds(6);
constexpr auto LISTEN_SETTLE = std::chrono::milliseconds(150);
constexpr auto AFTER_LISTEN = std::chrono::milliseconds(350);

using Binding = SettingsMenu::Binding;

bool is_trigger(const std::string &gamepad) {
    return gamepad == "lefttrigger" || gamepad == "righttrigger";
}

// a key list as the Controls page holds it: up to two keys, named as key_name writes them; a list with
// none usable gives `fallback` (as apply_bindings keeps the default for it)
std::vector<std::string> binding_keys(const std::string &list, const std::string &fallback) {
    std::vector<std::string> keys;
    const auto items = split_bindings(list);
    for (const auto &item : items) {
        const auto code = key_from_name(item);
        if (code != input::PhysicalKeyCode::Unbound && keys.size() < 2)
            keys.push_back(key_name(code));
    }
    if (keys.empty() && !items.empty() && list != fallback)
        return binding_keys(fallback, fallback);
    return keys;
}

// a gamepad list as the Controls page holds it: up to two, [gamepad]'s names (south ... north for the
// face buttons, lefttrigger, righttrigger); a list with none usable gives `fallback` (as apply_bindings
// keeps the binding for it)
std::vector<std::string> binding_gamepads(const std::string &list, const std::string &fallback) {
    std::vector<std::string> pads;
    const auto items = split_bindings(list);
    for (const auto &item : items) {
        std::string name = string_utils::tolower(item);
        if (!is_trigger(name)) {
            const int button = gamepad_button_from_name(name);
            name = button >= 0 ? gamepad_button_name(button) : std::string();
        }
        if (!name.empty() && pads.size() < 2 && std::find(pads.begin(), pads.end(), name) == pads.end())
            pads.push_back(name);
    }
    if (pads.empty() && !items.empty() && list != fallback)
        return binding_gamepads(fallback, fallback);
    return pads;
}

std::string join_gamepad_names(const std::vector<std::string> &pads) {
    std::string out;
    for (const auto &pad : pads)
        out += (out.empty() ? "" : ", ") + gamepad_display_name(pad);
    return out;
}

std::string join_keys(const std::vector<std::string> &keys) {
    std::string out;
    for (const auto &key : keys)
        out += (out.empty() ? "" : ", ") + key;
    return out;
}

std::string page_title(const std::string &section) {
    static const std::map<std::string, std::string> titles = {
        { "paths", "Paths" }, { "display", "Display" }, { "audio", "Audio" }, { "recompiled", "Native code" },
        { "debug", "Debug" }, { "game", "Game" }, { "multiplayer", "Multiplayer" }, { "controls", "Controls" },
    };
    const auto it = titles.find(section);
    if (it != titles.end())
        return it->second;
    std::string t = section;
    if (!t.empty())
        t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

// "window_scale" -> "Window scale"
std::string row_title(const std::string &key) {
    std::string t = key;
    std::replace(t.begin(), t.end(), '_', ' ');
    if (!t.empty())
        t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

label text(const std::string &s, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t size, color4f color,
    overlay_element::text_align align = overlay_element::left, bool bold = false) {
    label l;
    l.set_font(overlay::default_font_name, size, bold);
    l.fore_color = color;
    l.back_color = CLEAR;
    l.set_pos(x, y);
    l.set_size(w, h);
    l.align_text(align);
    l.set_text(s);
    return l;
}

rounded_rect rect(int16_t x, int16_t y, uint16_t w, uint16_t h, color4f color, uint16_t radius) {
    rounded_rect r;
    r.set_pos(x, y);
    r.set_size(w, h);
    r.back_color = color;
    r.border_radius = radius;
    return r;
}

std::string format_float(double v) {
    std::string s = fmt::format("{:.2f}", v);
    while (s.back() == '0')
        s.pop_back();
    if (s.back() == '.')
        s.pop_back();
    return s;
}

std::string shown_value(const SettingsMenu::Row &row) {
    if (!row.android_action.empty()) return "Open";
    // a choice with labels shows its label (an unset one its example's, as the default)
    if (!row.spec.choice_labels.empty()) {
        const std::string &value = row.value.empty() ? row.spec.value : row.value;
        for (size_t i = 0; i < row.spec.choices.size() && i < row.spec.choice_labels.size(); i++)
            if (row.spec.choices[i] == value)
                return row.spec.choice_labels[i] + (row.value.empty() ? " (default)" : "");
    }
    if (row.value.empty() && row.spec.text_entry)
        return "(none)";
    if (row.value.empty())
        return row.editable ? "(default)" : "(not set)";
    if (row.spec.kind == Kind::Bool)
        return row.value == "true" ? "On" : "Off";
    if (row.spec.key == "scale" || row.spec.key == "window_scale")
        return row.value + "x";
    if (row.spec.key == "on_screen_size" || row.spec.key == "on_screen_opacity")
        return row.value + "%";
    return row.value;
}

} // namespace

SettingsMenu::SettingsMenu(const IniLayout &layout, const Values &values, const std::string &game_name,
    const std::vector<TouchBinding> &touches, const std::vector<TouchBinding> &default_touches)
    : m_game_name(game_name) {
    visible = true;
    m_allow_input_on_pause = true;
    m_column = FIRST_COLUMN;
    m_mouse_camera_enabled = values.values.contains("mouse.camera") && values.get_bool("mouse.camera");
    m_mouse_capture_keys = binding_keys(values.get("mouse.capture_key"), "");
    const bool has_gamepad = std::any_of(layout.sections.begin(), layout.sections.end(), [](const IniSection &s) { return s.name == "gamepad"; });
    for (const auto &section : layout.sections) {
        if (section.name == "keyboard" && has_gamepad) {
            // the Controls page: [keyboard], [gamepad] and [touch], where [keyboard] is in the file (on
            // Android, on the Controls page the on-screen controls already have)
            std::vector<Row> rows;
            Row header;
            header.header = true;
            header.editable = false;
            rows.push_back(header);
            const auto &keyboard = values.section("keyboard");
            const auto &gamepad = values.section("gamepad");
            for (const auto &b : button_bindings()) {
#ifdef __ANDROID__
                // the gamepad only: nothing to change for what a gamepad's sticks and triggers drive
                if (*b.fixed_gamepad)
                    continue;
#endif
                Row row;
                row.binding = Binding::Button;
                row.section = "keyboard";
                row.name = b.name;
                row.spec.key = b.name;
                row.spec.title = b.title;
                row.default_keys = binding_keys(b.keys, b.keys);
                const auto k = keyboard.find(b.name);
                row.keys = k != keyboard.end() ? binding_keys(k->second, b.keys) : row.default_keys;
                row.fixed_gamepad = b.fixed_gamepad;
                if (row.fixed_gamepad.empty()) {
                    row.default_gamepads = binding_gamepads(b.gamepad, b.gamepad);
                    const auto g = gamepad.find(b.name);
                    row.gamepads = g != gamepad.end() ? binding_gamepads(g->second, b.gamepad) : row.default_gamepads;
                }
                const std::string name = row.name;
                if (name == "l2" || name == "r2")
                    row.spec.help = "A PS TV's " + string_utils::toupper(name) + ": only with PS TV controls (the Game page). On a gamepad: "
                        + row.fixed_gamepad + " (always; a Vita button can take it as well).";
                else if (!row.fixed_gamepad.empty())
                    row.spec.help = "On a gamepad: the " + string_utils::tolower(row.fixed_gamepad) + ".";
                rows.push_back(std::move(row));
            }
            for (const auto &touch : touches) {
                const auto def = std::find_if(default_touches.begin(), default_touches.end(), [&](const TouchBinding &t) { return t.name == touch.name; });
                Row row;
                row.binding = Binding::Touch;
                row.section = "touch";
                row.name = touch.name;
                row.spec.key = touch.name;
                row.spec.title = !touch.title.empty() ? touch.title
                    : def != default_touches.end() && !def->title.empty()      ? def->title
                                                                               : row_title(touch.name) + " (touch)";
                // a touch the game doesn't have by default resets to what it is
                row.default_keys = binding_keys(def != default_touches.end() ? def->keys : touch.keys, def != default_touches.end() ? def->keys : touch.keys);
                const std::string default_pads = def != default_touches.end() ? def->gamepad : touch.gamepad;
                row.default_gamepads = binding_gamepads(default_pads, default_pads);
                row.keys = binding_keys(touch.keys, def != default_touches.end() ? def->keys : touch.keys);
                row.gamepads = binding_gamepads(touch.gamepad, default_pads);
                row.spec.help = "Holds a finger on the screen for as long as it is held.";
                rows.push_back(std::move(row));
            }
            Row reset;
            reset.reset = true;
            reset.spec.title = "Reset all controls";
            reset.spec.help = "Puts every key and gamepad button on this page back to its default.";
            rows.push_back(std::move(reset));
            const auto controls = std::find_if(m_pages.begin(), m_pages.end(), [](const Page &p) { return p.title == "Controls"; });
            if (controls != m_pages.end())
                controls->rows.insert(controls->rows.end(), rows.begin(), rows.end());
            else
                m_pages.push_back({ "Controls", std::move(rows) });
            continue;
        }
        if (section.free_keys || section.settings.empty())
            continue;
        Page page{ page_title(section.name), {} };
        for (const auto &spec : section.settings) {
#ifdef __ANDROID__
            if (spec.desktop_only)
                continue;
#endif
            Row row;
            row.section = section.name;
            row.spec = spec;
            row.value = values.get(section.name + "." + spec.key);
            row.editable = spec.kind != Kind::Path && (spec.kind != Kind::Text || spec.text_entry);
            page.rows.push_back(std::move(row));
        }
#ifdef __ANDROID__
        const auto action = [&](const char *key, const char *title, const char *help) {
            Row row; row.android_action = key; row.spec.key = key; row.spec.title = title;
            row.spec.help = help; row.spec.live = true; row.editable = true;
            page.rows.push_back(std::move(row));
        };
        if (section.name == "debug") {
            action("diagnostic", "Share diagnostic report", "Share existing setup/runtime logs with device and application details.");
        }
#endif
        // a page with nothing to change from here (paths) stays in the settings file only
        if (std::any_of(page.rows.begin(), page.rows.end(), [](const Row &r) { return r.editable; }))
            m_pages.push_back(std::move(page));
    }
    // the last page: Exit the game (a launcher's or a Steam Deck's player has no other way out in
    // fullscreen), confirmed with a second Cross
    {
        Row row;
        row.spec.title = "Exit the game";
        row.spec.help = "Closes the game. Progress since the game last saved is lost; settings changed here are already saved.";
        row.exit = true;
        m_pages.push_back({ "Exit", { std::move(row) } });
    }
    // Controls after Audio (where Android's on-screen controls are), and Mouse after it
    const auto place_after = [this](const char *title, const char *after) {
        const auto find = [this](const char *t) {
            return std::find_if(m_pages.begin(), m_pages.end(), [&](const Page &p) { return p.title == t; });
        };
        auto page = find(title);
        if (page == m_pages.end() || find(after) == m_pages.end())
            return;
        Page moved = std::move(*page);
        m_pages.erase(page);
        m_pages.insert(find(after) + 1, std::move(moved));
    };
    place_after("Controls", "Audio");
    place_after("Mouse", "Controls");
    // start on Display, the page most changes are on
    for (size_t i = 0; i < m_pages.size(); i++)
        if (m_pages[i].title == "Display")
            m_page = i;
}

std::vector<SettingsMenu::Change> SettingsMenu::take_changes() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Change> out;
    out.swap(m_changes);
    return out;
}

void SettingsMenu::request_close() {
    m_close_requested.store(true);
}

void SettingsMenu::step(Row &row, int direction) {
    if (!row.android_action.empty()) { m_changes.push_back({ "android_action", row.android_action, "open" }); return; }
    if (!row.editable || row.exit)
        return;
    const SettingSpec &spec = row.spec;
    // an unset (commented) setting starts from its example value
    const std::string current = row.value.empty() ? spec.value : row.value;
    std::string next = current;
    switch (spec.kind) {
    case Kind::Bool:
        next = current == "true" ? "false" : "true";
        break;
    case Kind::Choice: {
        if (spec.choices.empty())
            return;
        size_t i = 0;
        for (size_t k = 0; k < spec.choices.size(); k++)
            if (string_utils::tolower(spec.choices[k]) == string_utils::tolower(current))
                i = k;
        const size_t n = spec.choices.size();
        next = spec.choices[(i + n + (direction < 0 ? n - 1 : 1)) % n];
        break;
    }
    case Kind::Int: {
        const int step = spec.max - spec.min >= 50 ? 5 : 1;
        const int v = std::clamp(std::atoi(current.c_str()) + direction * step, static_cast<int>(spec.min), static_cast<int>(spec.max));
        next = std::to_string(v);
        break;
    }
    case Kind::Float: {
        const double step = spec.max - spec.min >= 4 ? 0.5 : 0.1;
        double v = std::atof(current.c_str()) + direction * step;
        v = std::clamp(std::round(v / step) * step, spec.min, spec.max);
        next = format_float(v);
        break;
    }
    case Kind::Text:
        // Cross or Right starts editing it (edit_button, text_input); Left does nothing
        if (spec.text_entry && direction > 0) {
            m_edit_text = row.value;
            m_edit_cursor = m_edit_text.size();
            m_text_editing.store(true);
        }
        return;
    default:
        return;
    }
    if (next == row.value)
        return;
    row.value = next;
    if (row.section == "mouse" && spec.key == "camera")
        m_mouse_camera_enabled = next == "true";
    if (!spec.live)
        row.needs_restart = true;
    m_changes.push_back({ row.section, spec.key, next });
}

SettingsMenu::Row *SettingsMenu::selected_row() {
    if (m_pages.empty() || m_row >= m_pages[m_page].rows.size())
        return nullptr;
    return &m_pages[m_page].rows[m_row];
}

void SettingsMenu::end_edit(bool commit) {
    if (!m_text_editing.load())
        return;
    m_text_editing.store(false);
    Row *row = selected_row();
    if (!commit || !row)
        return;
    // spaces around it go; the separators (commas) stay as typed
    std::string next = m_edit_text;
    next.erase(0, next.find_first_not_of(' '));
    next.erase(next.find_last_not_of(' ') + 1);
    if (next == row->value)
        return;
    row->value = next;
    if (!row->spec.live)
        row->needs_restart = true;
    m_changes.push_back({ row->section, row->spec.key, next });
}

// a gamepad while a row is edited: Up/Down change the character at the cursor (through the row's
// text_chars), Left/Right move the cursor, Square deletes, Cross/Start keep the text, Circle drops it
void SettingsMenu::edit_button(pad_button button) {
    const Row *row = selected_row();
    const std::string chars = row && !row->spec.text_chars.empty() ? row->spec.text_chars : "0123456789.";
    switch (button) {
    case pad_button::dpad_up:
    case pad_button::ls_up:
    case pad_button::dpad_down:
    case pad_button::ls_down: {
        const int direction = button == pad_button::dpad_up || button == pad_button::ls_up ? 1 : -1;
        if (m_edit_cursor >= m_edit_text.size()) {
            m_edit_text.push_back(direction > 0 ? chars.front() : chars.back());
            m_edit_cursor = m_edit_text.size() - 1;
            break;
        }
        const size_t at = chars.find(static_cast<char>(std::tolower(static_cast<unsigned char>(m_edit_text[m_edit_cursor]))));
        const size_t n = chars.size();
        const size_t next = at == std::string::npos ? 0 : (at + n + (direction > 0 ? 1 : n - 1)) % n;
        m_edit_text[m_edit_cursor] = chars[next];
        break;
    }
    case pad_button::dpad_left:
    case pad_button::ls_left:
        if (m_edit_cursor > 0)
            m_edit_cursor--;
        break;
    case pad_button::dpad_right:
    case pad_button::ls_right:
        if (m_edit_cursor < m_edit_text.size())
            m_edit_cursor++;
        break;
    case pad_button::square:
        if (m_edit_cursor < m_edit_text.size())
            m_edit_text.erase(m_edit_cursor, 1);
        else if (!m_edit_text.empty()) {
            m_edit_text.pop_back();
            m_edit_cursor = m_edit_text.size();
        }
        break;
    case pad_button::cross:
    case pad_button::start:
        end_edit(true);
        break;
    case pad_button::circle:
        end_edit(false);
        break;
    default:
        break;
    }
}

void SettingsMenu::text_input(const std::string &text) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_text_editing.load())
            return;
        for (const char c : text) {
            if (c < 0x20 || c > 0x7E || m_edit_text.size() >= 200)
                continue; // printable ASCII only (an address or a host name)
            m_edit_text.insert(m_edit_cursor++, 1, c);
        }
    }
    refresh();
}

void SettingsMenu::text_key(TextKey key) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_text_editing.load())
            return;
        switch (key) {
        case TextKey::Backspace:
            if (m_edit_cursor > 0)
                m_edit_text.erase(--m_edit_cursor, 1);
            break;
        case TextKey::Delete:
            if (m_edit_cursor < m_edit_text.size())
                m_edit_text.erase(m_edit_cursor, 1);
            break;
        case TextKey::Left:
            if (m_edit_cursor > 0)
                m_edit_cursor--;
            break;
        case TextKey::Right:
            if (m_edit_cursor < m_edit_text.size())
                m_edit_cursor++;
            break;
        case TextKey::Home:
            m_edit_cursor = 0;
            break;
        case TextKey::End:
            m_edit_cursor = m_edit_text.size();
            break;
        case TextKey::Enter:
            end_edit(true);
            break;
        case TextKey::Escape:
            end_edit(false);
            break;
        }
    }
    refresh();
}

void SettingsMenu::move_row(int direction) {
    if (m_pages.empty())
        return;
    const auto &rows = m_pages[m_page].rows;
    if (rows.empty())
        return;
    m_note.clear();
    // past the column names, which are never selected
    for (size_t tries = 0; tries < rows.size(); tries++) {
        m_row = (m_row + rows.size() + (direction < 0 ? rows.size() - 1 : 1)) % rows.size();
        if (!rows[m_row].header)
            break;
    }
    if (m_row < m_scroll)
        m_scroll = m_row;
    // the column names stay in sight above the first binding
    if (m_row == m_scroll && m_row > 0 && rows[m_row - 1].header)
        m_scroll = m_row - 1;
    if (m_row >= m_scroll + VISIBLE_ROWS)
        m_scroll = m_row - VISIBLE_ROWS + 1;
}

void SettingsMenu::move_page(int direction) {
    if (m_pages.empty())
        return;
    m_page = (m_page + m_pages.size() + (direction < 0 ? m_pages.size() - 1 : 1)) % m_pages.size();
    m_row = 0;
    m_scroll = 0;
    m_note.clear();
    const auto &rows = m_pages[m_page].rows;
    while (m_row + 1 < rows.size() && rows[m_row].header)
        m_row++;
}

std::vector<SettingsMenu::Row *> SettingsMenu::binding_rows() {
    std::vector<Row *> out;
    for (auto &page : m_pages)
        for (auto &row : page.rows)
            if (row.binding != Binding::None)
                out.push_back(&row);
    return out;
}

void SettingsMenu::set_keys(Row &row, std::vector<std::string> keys) {
    if (keys.size() > 2)
        keys.resize(2);
    if (keys == row.keys)
        return;
    row.keys = keys;
    // the button that made the change may still be held, and now press another Vita button
    m_ignore_until = std::max(m_ignore_until, std::chrono::steady_clock::now() + AFTER_LISTEN);
    if (row.binding == Binding::Button)
        m_changes.push_back({ "keyboard", row.name, join_keys(keys) });
    else
        m_changes.push_back({ "touch", row.name + "_key", join_keys(keys) });
}

void SettingsMenu::set_gamepads(Row &row, std::vector<std::string> gamepads) {
    if (gamepads.size() > 2)
        gamepads.resize(2);
    if (gamepads == row.gamepads || !row.fixed_gamepad.empty())
        return;
    row.gamepads = gamepads;
    m_ignore_until = std::max(m_ignore_until, std::chrono::steady_clock::now() + AFTER_LISTEN);
    if (row.binding == Binding::Button)
        m_changes.push_back({ "gamepad", row.name, join_keys(gamepads) });
    else
        m_changes.push_back({ "touch", row.name + "_gamepad", join_keys(gamepads) });
}

// A key into one of a row's two key cells. A key another binding has changes places with this cell's
// old one (that binding gets it, or loses the key when the cell was empty), so no binding is lost by
// accident and no key presses two things.
void SettingsMenu::assign_key(Row &row, size_t column, const std::string &key) {
    const auto same = [](const std::string &a, const std::string &b) { return key_from_name(a) == key_from_name(b); };
    std::vector<std::string> keys = row.keys;
    for (size_t i = 0; i < keys.size(); i++) {
        if (!same(keys[i], key))
            continue;
        // the row's other key: the two change places
        if (column < keys.size() && i != column)
            std::swap(keys[i], keys[column]);
        set_keys(row, keys);
        return;
    }
    const std::string old = column < keys.size() ? keys[column] : std::string();
    for (Row *other : binding_rows()) {
        if (other == &row)
            continue;
        std::vector<std::string> other_keys = other->keys;
        const auto it = std::find_if(other_keys.begin(), other_keys.end(), [&](const std::string &k) { return same(k, key); });
        if (it == other_keys.end())
            continue;
        const bool has_old = !old.empty() && std::any_of(other_keys.begin(), other_keys.end(), [&](const std::string &k) { return same(k, old); });
        if (old.empty() || has_old)
            other_keys.erase(it);
        else
            *it = old;
        set_keys(*other, other_keys);
        m_note = old.empty() || has_old ? fmt::format("{} was on {}: no longer there.", key, other->spec.title)
                                        : fmt::format("{} was on {}: {} is there now.", key, other->spec.title, old);
    }
    if (column < keys.size())
        keys[column] = key;
    else
        keys.push_back(key);
    set_keys(row, keys);
    if (m_note.empty() && m_mouse_camera_enabled
        && std::any_of(m_mouse_capture_keys.begin(), m_mouse_capture_keys.end(), [&](const auto &capture) { return same(key, capture); }))
        m_note = key + " now has a game binding. Middle-click can still capture the cursor.";
}

// A gamepad button or trigger into one of a row's two gamepad cells (slot 0, 1), as assign_key does it
// for keys: one another binding has changes places with this cell's old one
void SettingsMenu::assign_gamepad(Row &row, size_t slot, const std::string &gamepad) {
    if (!row.fixed_gamepad.empty())
        return;
    std::vector<std::string> pads = row.gamepads;
    for (size_t i = 0; i < pads.size(); i++) {
        if (pads[i] != gamepad)
            continue;
        // the row's other one: the two change places
        if (slot < pads.size() && i != slot)
            std::swap(pads[i], pads[slot]);
        set_gamepads(row, pads);
        return;
    }
    const std::string old = slot < pads.size() ? pads[slot] : std::string();
    for (Row *other : binding_rows()) {
        if (other == &row || !other->fixed_gamepad.empty())
            continue;
        std::vector<std::string> other_pads = other->gamepads;
        const auto it = std::find(other_pads.begin(), other_pads.end(), gamepad);
        if (it == other_pads.end())
            continue;
        const bool has_old = !old.empty() && std::find(other_pads.begin(), other_pads.end(), old) != other_pads.end();
        if (old.empty() || has_old)
            other_pads.erase(it);
        else
            *it = old;
        set_gamepads(*other, other_pads);
        m_note = old.empty() || has_old ? fmt::format("{} was on {}: no longer there.", gamepad_display_name(gamepad), other->spec.title)
                                        : fmt::format("{} was on {}: {} is there now.", gamepad_display_name(gamepad), other->spec.title,
                                              gamepad_display_name(old));
    }
    if (slot < pads.size())
        pads[slot] = gamepad;
    else
        pads.push_back(gamepad);
    set_gamepads(row, pads);
    // the triggers press a PS TV's L2 and R2 whatever else they are (ctrl.cpp, apply_controller)
    if (is_trigger(gamepad) && m_note.empty())
        m_note = fmt::format("{} stays a PS TV's {} as well.", gamepad_display_name(gamepad), gamepad == "lefttrigger" ? "L2" : "R2");
}

void SettingsMenu::reset_bindings() {
    for (Row *row : binding_rows()) {
        set_keys(*row, row->default_keys);
        set_gamepads(*row, row->default_gamepads);
    }
    m_note = "Every control is back to its default.";
}

void SettingsMenu::end_listen() {
    m_listening.store(false);
    m_ignore_until = std::chrono::steady_clock::now() + AFTER_LISTEN;
}

void SettingsMenu::listen_timeout() {
    if (m_listening.load() && std::chrono::steady_clock::now() - m_listen_start > LISTEN_TIMEOUT)
        m_listening.store(false);
}

bool SettingsMenu::listen_ready() const {
    return std::chrono::steady_clock::now() - m_listen_start >= LISTEN_SETTLE;
}

// a Controls page row: Left/Right choose the column, Cross waits for the new key or button, Square
// clears the cell, Triangle puts the row back to its defaults
void SettingsMenu::binding_button(Row &row, pad_button button) {
    switch (button) {
    case pad_button::dpad_left:
    case pad_button::ls_left:
        if (m_column > FIRST_COLUMN)
            m_column--;
        break;
    case pad_button::dpad_right:
    case pad_button::ls_right:
        if (m_column + 1 < COLUMNS)
            m_column++;
        break;
    case pad_button::cross:
    case pad_button::start:
        if (m_column >= PAD_COLUMN && !row.fixed_gamepad.empty()) {
            m_note = "The gamepad's sticks and triggers can't be changed.";
            break;
        }
        m_note.clear();
        m_listen_start = std::chrono::steady_clock::now();
        m_listening.store(true);
        break;
    case pad_button::square:
        if (m_column < PAD_COLUMN) {
            std::vector<std::string> keys = row.keys;
            if (m_column < keys.size())
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(m_column));
            set_keys(row, keys);
        } else {
            std::vector<std::string> pads = row.gamepads;
            if (m_column - PAD_COLUMN < pads.size())
                pads.erase(pads.begin() + static_cast<std::ptrdiff_t>(m_column - PAD_COLUMN));
            set_gamepads(row, pads);
        }
        break;
    case pad_button::triangle: {
        m_note.clear();
        if (row.default_keys.empty())
            set_keys(row, {});
        else {
            assign_key(row, 0, row.default_keys[0]);
            if (row.default_keys.size() > 1)
                assign_key(row, 1, row.default_keys[1]);
            else if (row.keys.size() > 1)
                set_keys(row, { row.keys[0] });
        }
        if (row.default_gamepads.empty())
            set_gamepads(row, {});
        else {
            assign_gamepad(row, 0, row.default_gamepads[0]);
            if (row.default_gamepads.size() > 1)
                assign_gamepad(row, 1, row.default_gamepads[1]);
            else if (row.gamepads.size() > 1)
                set_gamepads(row, { row.gamepads[0] });
        }
        break;
    }
    default:
        break;
    }
}

void SettingsMenu::bind_key(int scancode) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listen_timeout();
        Row *row = selected_row();
        if (!m_listening.load() || !listen_ready() || !row || row->binding == Binding::None)
            return;
        const auto code = key_from_scancode(scancode);
        const std::string w3c(input::physical_key_code_name(code));
        if (w3c == "Escape" || m_column >= PAD_COLUMN)
            end_listen(); // cancelled (a key while a gamepad button is awaited too)
        else if (w3c.empty() || w3c == "F11" || w3c == "F12")
            return; // the front end's own keys (F1 closes the menu before it gets here): waiting on
        else {
            end_listen();
            assign_key(*row, m_column, key_name(code));
        }
    }
    refresh();
}

void SettingsMenu::bind_mouse(int sdl_button) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listen_timeout();
        Row *row = selected_row();
        if (!m_listening.load() || !listen_ready() || !row || row->binding == Binding::None || m_column >= PAD_COLUMN)
            return;
        // the middle button captures the cursor: no binding
        if (sdl_button != SDL_BUTTON_LEFT && sdl_button != SDL_BUTTON_RIGHT && sdl_button != SDL_BUTTON_X1 && sdl_button != SDL_BUTTON_X2)
            return;
        end_listen();
        assign_key(*row, m_column, key_name(mouse_button_code(sdl_button)));
        if (m_note.empty() && (sdl_button == SDL_BUTTON_LEFT || sdl_button == SDL_BUTTON_RIGHT))
            m_note = key_name(mouse_button_code(sdl_button)) + " presses it while the mouse camera has the cursor (otherwise it touches the screen).";
    }
    refresh();
}

void SettingsMenu::bind_gamepad_button(int sdl_button) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listen_timeout();
        Row *row = selected_row();
        if (!m_listening.load() || !listen_ready() || !row || row->binding == Binding::None)
            return;
        end_listen();
        if (m_column >= PAD_COLUMN) // on a key cell, a gamepad button cancels
            assign_gamepad(*row, m_column - PAD_COLUMN, gamepad_button_name(sdl_button));
    }
    refresh();
}

void SettingsMenu::bind_gamepad_trigger(int sdl_axis) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listen_timeout();
        Row *row = selected_row();
        if (!m_listening.load() || !listen_ready() || !row || row->binding == Binding::None)
            return;
        end_listen();
        if (m_column >= PAD_COLUMN) // on a key cell, a trigger cancels
            assign_gamepad(*row, m_column - PAD_COLUMN, sdl_axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
    }
    refresh();
}

void SettingsMenu::on_button_pressed(pad_button button, bool is_auto_repeat) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // a binding waiting for its input, and a moment after: the menu's own buttons are that input (a
        // gamepad's, which reach the menu as well), and so are their repeats while held
        listen_timeout();
        if (m_listening.load() || std::chrono::steady_clock::now() < m_ignore_until) {
            m_swallowed = button;
            goto done;
        }
        if (is_auto_repeat && button == m_swallowed)
            goto done;
        m_swallowed = pad_button::pad_button_max_enum;
        if (m_text_editing.load()) {
            edit_button(button);
            goto done;
        }
        Row *row = nullptr;
        if (!m_pages.empty() && m_row < m_pages[m_page].rows.size())
            row = &m_pages[m_page].rows[m_row];
        if (row && !row->android_action.empty() && (button == pad_button::cross || button == pad_button::start || button == pad_button::dpad_left || button == pad_button::dpad_right || button == pad_button::ls_left || button == pad_button::ls_right)) {
            if (!is_auto_repeat && (button == pad_button::cross || button == pad_button::start)) step(*row, 1);
            goto done;
        }
        // Exit the game: Cross (or Start) once arms it, a second time exits; any other button disarms.
        // Reset all controls the same way.
        const bool confirm = button == pad_button::cross || button == pad_button::start;
        if (row && row->exit && confirm) {
            if (m_exit_armed)
                m_exit_requested.store(true);
            m_exit_armed = true;
            button = pad_button::pad_button_max_enum; // handled
        } else
            m_exit_armed = false;
        if (row && row->reset && confirm) {
            if (m_reset_armed)
                reset_bindings();
            m_reset_armed = !m_reset_armed;
            button = pad_button::pad_button_max_enum;
        } else
            m_reset_armed = false;
        if (row && row->binding != Binding::None) {
            switch (button) {
            case pad_button::dpad_left:
            case pad_button::ls_left:
            case pad_button::dpad_right:
            case pad_button::ls_right:
            case pad_button::cross:
            case pad_button::start:
            case pad_button::square:
            case pad_button::triangle:
                binding_button(*row, button);
                goto done;
            default:
                break;
            }
        }
        switch (button) {
        case pad_button::dpad_up:
        case pad_button::ls_up:
            move_row(-1);
            break;
        case pad_button::dpad_down:
        case pad_button::ls_down:
            move_row(1);
            break;
        case pad_button::dpad_left:
        case pad_button::ls_left:
            if (row)
                step(*row, -1);
            break;
        case pad_button::dpad_right:
        case pad_button::ls_right:
        case pad_button::cross:
        case pad_button::start:
            if (row)
                step(*row, 1);
            break;
        case pad_button::L1:
        case pad_button::L2:
            move_page(-1);
            break;
        case pad_button::R1:
        case pad_button::R2:
            move_page(1);
            break;
        case pad_button::circle:
            request_close();
            break;
        default:
            break;
        }
    }
done:
    refresh();
}

::overlay::compiled_resource SettingsMenu::get_compiled() {
    ::overlay::compiled_resource result;
    if (!visible)
        return result;
    std::lock_guard<std::mutex> lock(m_mutex);
    listen_timeout();
    const bool listening = m_listening.load();
    // the key a Vita button is on now (the Controls page changes them), for the hints
    const auto bound = [&](const char *name, const char *fallback) -> std::string {
        for (const auto &page : m_pages)
            for (const auto &row : page.rows)
                if (row.binding == Binding::Button && row.name == name)
                    return row.keys.empty() ? "-" : row.keys[0];
        return fallback;
    };

    result.add(rect(0, 0, virtual_width, virtual_height, { 0.f, 0.f, 0.f, 0.55f }, 0).get_compiled());
    result.add(rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL, 10).get_compiled());
    result.add(text("Settings", PANEL_X + 24, PANEL_Y + 12, 300, 30, 22, TEXT, overlay_element::left, true).get_compiled());
    result.add(text(m_game_name, PANEL_X + 324, PANEL_Y + 18, PANEL_W - 348, 24, 14, TEXT_DIM, overlay_element::right).get_compiled());

    // the pages, L1/R1: each tab as wide as its name (bold, as the current one is drawn) plus an even
    // share of the room left; a smaller font when seven or more names don't fit at 15
    if (!m_pages.empty()) {
        constexpr uint16_t TABS_W = PANEL_W - 48, MIN_GAP = 10;
        std::vector<uint16_t> widths(m_pages.size());
        uint16_t size = 15;
        uint32_t sum = 0;
        for (;; size--) {
            sum = 0;
            for (size_t i = 0; i < m_pages.size(); i++) {
                uint16_t w = 0, h = 0;
                text(m_pages[i].title, 0, 0, TABS_W, 26, size, TEXT, overlay_element::left, true).measure_text(w, h, true);
                widths[i] = w;
                sum += w;
            }
            if (sum + MIN_GAP * m_pages.size() <= TABS_W || size <= 11)
                break;
        }
        const uint16_t gap = static_cast<uint16_t>(sum < TABS_W ? (TABS_W - sum) / m_pages.size() : 0);
        int16_t x = PANEL_X + 24;
        for (size_t i = 0; i < m_pages.size(); i++) {
            const uint16_t tab_w = static_cast<uint16_t>(widths[i] + gap);
            if (i == m_page)
                result.add(rect(x + 4, TABS_Y + 28, tab_w - 8, 3, ACCENT, 1).get_compiled());
            result.add(text(m_pages[i].title, x, TABS_Y, tab_w, 26, size, i == m_page ? TEXT : TEXT_DIM, overlay_element::center, i == m_page).get_compiled());
            x = static_cast<int16_t>(x + tab_w);
        }
    }

    const Page *page = m_pages.empty() ? nullptr : &m_pages[m_page];
    bool any_restart = false;
    if (page) {
        for (const auto &row : page->rows)
            any_restart = any_restart || row.needs_restart;
        for (size_t i = m_scroll; i < page->rows.size() && i < m_scroll + VISIBLE_ROWS; i++) {
            const Row &row = page->rows[i];
            const int16_t y = static_cast<int16_t>(ROWS_Y + ROW_H * static_cast<int16_t>(i - m_scroll));
            const bool selected = i == m_row;
            if (row.header) {
                // the Controls page's column names
                result.add(text("Vita button", PANEL_X + 34, y + 12, 200, 20, 13, TEXT_DIM).get_compiled());
                static const char *const names[] = { "Key", "Second key", "Gamepad", "Second button" };
                for (size_t c = FIRST_COLUMN; c < COLUMNS; c++)
                    result.add(text(names[c], cell_x(c), y + 12, CELL_W, 20, 13, TEXT_DIM, overlay_element::center).get_compiled());
                continue;
            }
            if (selected)
                result.add(rect(PANEL_X + 16, y, PANEL_W - 32, ROW_H - 4, SELECTED, 6).get_compiled());
            if (row.binding != Binding::None) {
                // a binding: its name, its two keys and its two gamepad buttons, the selected one framed
                result.add(text(row.spec.title, PANEL_X + 34, y + 6, 210, 24, 16, TEXT).get_compiled());
                for (size_t c = FIRST_COLUMN; c < COLUMNS; c++) {
                    const bool cell_selected = selected && c == m_column;
                    const bool waiting = cell_selected && listening;
                    std::string value;
                    bool dim = false;
                    if (waiting)
                        value = c < PAD_COLUMN ? "Press a key" : "Press it";
                    else if (c < PAD_COLUMN) {
                        dim = c >= row.keys.size();
                        value = dim ? "-" : row.keys[c];
                    } else if (!row.fixed_gamepad.empty()) {
                        // a stick or a trigger, in the first gamepad cell
                        value = c == PAD_COLUMN ? row.fixed_gamepad : std::string();
                        dim = true;
                    } else {
                        const size_t slot = c - PAD_COLUMN;
                        dim = slot >= row.gamepads.size();
                        value = dim ? "-" : gamepad_display_name(row.gamepads[slot]);
                    }
                    if (cell_selected)
                        result.add(rect(cell_x(c), y + 2, CELL_W, ROW_H - 8, waiting ? ACCENT : color4f{ 0.85f, 0.30f, 0.30f, 0.45f }, 5).get_compiled());
                    label cell = text(value, cell_x(c) + 4, y + 7, CELL_W - 8, 24, 15, dim ? TEXT_DIM : TEXT, overlay_element::center);
                    uint16_t cell_w = 0, cell_h = 0;
                    cell.measure_text(cell_w, cell_h, true);
                    while (cell_w > CELL_W - 8 && value.size() > 1) {
                        value.pop_back();
                        cell.set_text(value + "...");
                        cell.measure_text(cell_w, cell_h, true);
                    }
                    result.add(cell.get_compiled());
                }
                continue;
            }
            result.add(text(row.spec.title.empty() ? row_title(row.spec.key) : row.spec.title, PANEL_X + 34, y + 6, 330, 24, 16, TEXT).get_compiled());
            // the value, shortened with "..." to fit its column (a network adapter's long name)
            const bool confirmed = row.exit || row.reset; // Cross twice
            const bool armed = (row.exit && m_exit_armed) || (row.reset && m_reset_armed);
            const std::string arrows_l = row.editable && !confirmed && selected && row.spec.kind != Kind::Bool ? "<  " : "";
            const std::string arrows_r = (arrows_l.empty() ? "" : "  >") + std::string(row.needs_restart ? "  *" : "");
            constexpr uint16_t VALUE_W = PANEL_W - 284;
            std::string value = !confirmed ? shown_value(row)
                : !selected                ? ""
                : armed                    ? std::string(row.exit ? "Cross again to exit" : "Cross again to reset")
                                           : fmt::format("Cross ({})", bound("cross", "X"));
            const bool editing = selected && m_text_editing.load();
            if (editing) {
                // the text being edited, the cursor's character in brackets ([ ] at the end: a new one)
                value = m_edit_text.substr(0, m_edit_cursor) + "["
                    + (m_edit_cursor < m_edit_text.size() ? m_edit_text.substr(m_edit_cursor, 1) : std::string(" ")) + "]"
                    + (m_edit_cursor < m_edit_text.size() ? m_edit_text.substr(m_edit_cursor + 1) : std::string());
            }
            label value_label = text(editing ? value : arrows_l + value + arrows_r, PANEL_X + 250, y + 6, VALUE_W, 24, 16,
                armed ? ACCENT : row.editable ? TEXT : TEXT_DIM, overlay_element::right);
            uint16_t value_w = 0, value_h = 0;
            value_label.measure_text(value_w, value_h, true);
            while (value_w > VALUE_W && !value.empty()) {
                // an edited text loses its start (the cursor is usually at the end), any other its end
                if (editing) {
                    value.erase(0, 1);
                    value_label.set_text("..." + value);
                } else {
                    value.pop_back();
                    value_label.set_text(arrows_l + value + "..." + arrows_r);
                }
                value_label.measure_text(value_w, value_h, true);
            }
            result.add(value_label.get_compiled());
        }
        // more rows above or below
        if (m_scroll > 0)
            result.add(text("...", PANEL_X + PANEL_W - 60, ROWS_Y - 18, 40, 16, 12, TEXT_DIM, overlay_element::right).get_compiled());
        if (m_scroll + VISIBLE_ROWS < page->rows.size())
            result.add(text("...", PANEL_X + PANEL_W - 60, HELP_Y - 12, 40, 16, 12, TEXT_DIM, overlay_element::right).get_compiled());
    }

    // the selected setting's help, and what needs a restart
    std::string help;
    if (page && m_row < page->rows.size()) {
        const Row &row = page->rows[m_row];
        help = row.spec.help;
        std::replace(help.begin(), help.end(), '\n', ' ');
        if (row.binding != Binding::None && listening) {
            if (m_column < PAD_COLUMN)
                help = "Press the new key, or a mouse button: a side button, or the left or right button (those press it only "
                       "while the mouse camera has the cursor). Esc or a gamepad button: cancel.";
            else
                help = "Press the new gamepad button, or LT or RT. " CANCEL_KEY ": cancel.";
        } else if (row.binding != Binding::None) {
#ifdef __ANDROID__
            const std::string defaults = row.default_gamepads.empty() ? "none" : join_gamepad_names(row.default_gamepads);
            help += (help.empty() ? "" : " ") + fmt::format("Left/Right: the first or second button. Cross: change it, then press the new button "
                                                            "(LT and RT too). Square: clear it. Triangle: its defaults ({}). "
                                                            "A button another control has changes places with it.",
                                                    defaults);
#else
            std::string defaults = join_keys(row.default_keys);
            if (row.fixed_gamepad.empty() && !row.default_gamepads.empty())
                defaults += (defaults.empty() ? "" : ", ") + join_gamepad_names(row.default_gamepads);
            help += (help.empty() ? "" : " ") + fmt::format("Cross: change the selected one, then press the new key or button. Square: clear it. "
                                                            "Triangle: this control's defaults ({}). A key or button another control has changes places with it.",
                                                    defaults.empty() ? "none" : defaults);
#endif
        } else if (!row.editable)
            help += (help.empty() ? "" : " ") + std::string("Change it in the settings file.");
        else if (row.spec.text_entry && !m_text_editing.load())
            help += (help.empty() ? "" : " ") + std::string("Cross: edit.");
    }
    // the help gets the space down to the note (what a binding did, or what needs a restart) or the
    // hints; a longer one is cut at a word
    const int16_t note_y = FOOTER_Y - 24;
    const bool any_note = any_restart || !m_note.empty();
    const uint16_t help_h = static_cast<uint16_t>((any_note ? note_y - 4 : FOOTER_Y - 6) - HELP_Y);
    label help_label = text(help, PANEL_X + 30, HELP_Y, PANEL_W - 60, help_h, 13, TEXT_DIM);
    help_label.set_wrap_text(true);
    uint16_t text_w = 0, text_h = 0;
    help_label.measure_text(text_w, text_h);
    for (std::string cut = help; text_h > help_h;) {
        const auto space = cut.rfind(' ');
        if (space == std::string::npos) {
            help_label.set_text("");
            break;
        }
        cut.resize(space);
        help_label.set_text(cut + " ...");
        help_label.measure_text(text_w, text_h);
    }
    result.add(help_label.get_compiled());
    if (!m_note.empty())
        result.add(text(m_note, PANEL_X + 30, note_y, PANEL_W - 60, 18, 13, ACCENT).get_compiled());
    else if (any_restart)
        result.add(text("* takes effect the next time the game starts", PANEL_X + 30, note_y, PANEL_W - 60, 18, 13, ACCENT).get_compiled());

    // the controls: a gamepad's, and on a PC the keyboard's too (as the Controls page binds them)
    const bool on_binding = page && m_row < page->rows.size() && page->rows[m_row].binding != Binding::None;
#ifdef __ANDROID__
    const std::string menu_hints = on_binding ? "Left/Right: column    Cross: change    Square: clear    Triangle: default    L1/R1: page    Circle: close"
                                              : "Up/Down: choose    Left/Right, Cross: change    L1/R1: page    Circle: close";
#else
    const std::string menu_hints = on_binding
        ? fmt::format("Left/Right: column    Cross/Enter: change    Square: clear    Triangle: default    L1/R1 ({}/{}): page    Circle ({}), F1: close",
              bound("l", "Q"), bound("r", "E"), bound("circle", "C"))
        : fmt::format("Up/Down: choose    Left/Right, Cross/Enter: change    L1/R1 ({}/{}): page    Circle ({}), Esc, F1: close",
              bound("l", "Q"), bound("r", "E"), bound("circle", "C"));
#endif
    const std::string hints = m_text_editing.load()
        ? "Type, or Up/Down: character    Left/Right: move    Square: delete    Cross, Enter: done    Circle, Esc: cancel"
        : on_binding && listening ? std::string(m_column < PAD_COLUMN ? "Press the new key or mouse button    Esc: cancel" : "Press the new gamepad button, LT or RT    " CANCEL_KEY ": cancel")
                                  : menu_hints;
    result.add(text(hints, PANEL_X + 24, FOOTER_Y, PANEL_W - 48, 20, 12, TEXT_DIM, overlay_element::center).get_compiled());
    return result;
}

} // namespace game_frontend
