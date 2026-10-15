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

#include <overlay/controls.h>
#include <overlay/font.h>
#include <util/string_utils.h>

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

std::string page_title(const std::string &section) {
    static const std::map<std::string, std::string> titles = {
        { "paths", "Paths" }, { "display", "Display" }, { "audio", "Audio" }, { "recompiled", "Native code" },
        { "debug", "Debug" }, { "game", "Game" }, { "multiplayer", "Multiplayer" },
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
    // a choice with labels shows its label (an unset one its example's, as the default)
    if (!row.spec.choice_labels.empty()) {
        const std::string &value = row.value.empty() ? row.spec.value : row.value;
        for (size_t i = 0; i < row.spec.choices.size() && i < row.spec.choice_labels.size(); i++)
            if (row.spec.choices[i] == value)
                return row.spec.choice_labels[i] + (row.value.empty() ? " (default)" : "");
    }
    if (row.value.empty())
        return row.editable ? "(default)" : "(not set)";
    if (row.spec.kind == Kind::Bool)
        return row.value == "true" ? "On" : "Off";
    if (row.spec.key == "scale" || row.spec.key == "window_scale")
        return row.value + "x";
    return row.value;
}

} // namespace

SettingsMenu::SettingsMenu(const IniLayout &layout, const Values &values, const std::string &game_name)
    : m_game_name(game_name) {
    visible = true;
    m_allow_input_on_pause = true;
    for (const auto &section : layout.sections) {
        if (section.free_keys || section.settings.empty())
            continue;
        Page page{ page_title(section.name), {} };
        for (const auto &spec : section.settings) {
            Row row;
            row.section = section.name;
            row.spec = spec;
            row.value = values.get(section.name + "." + spec.key);
            row.editable = spec.kind != Kind::Path && spec.kind != Kind::Text;
            page.rows.push_back(std::move(row));
        }
        // a page with nothing to change from here (paths) stays in the settings file only
        if (std::any_of(page.rows.begin(), page.rows.end(), [](const Row &r) { return r.editable; }))
            m_pages.push_back(std::move(page));
    }
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
    if (!row.editable)
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
    default:
        return;
    }
    if (next == row.value)
        return;
    row.value = next;
    if (!spec.live)
        row.needs_restart = true;
    m_changes.push_back({ row.section, spec.key, next });
}

void SettingsMenu::move_row(int direction) {
    if (m_pages.empty())
        return;
    const auto &rows = m_pages[m_page].rows;
    if (rows.empty())
        return;
    m_row = (m_row + rows.size() + (direction < 0 ? rows.size() - 1 : 1)) % rows.size();
    if (m_row < m_scroll)
        m_scroll = m_row;
    if (m_row >= m_scroll + VISIBLE_ROWS)
        m_scroll = m_row - VISIBLE_ROWS + 1;
}

void SettingsMenu::move_page(int direction) {
    if (m_pages.empty())
        return;
    m_page = (m_page + m_pages.size() + (direction < 0 ? m_pages.size() - 1 : 1)) % m_pages.size();
    m_row = 0;
    m_scroll = 0;
}

void SettingsMenu::on_button_pressed(pad_button button, bool /*is_auto_repeat*/) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Row *row = nullptr;
        if (!m_pages.empty() && m_row < m_pages[m_page].rows.size())
            row = &m_pages[m_page].rows[m_row];
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
    refresh();
}

::overlay::compiled_resource SettingsMenu::get_compiled() {
    ::overlay::compiled_resource result;
    if (!visible)
        return result;
    std::lock_guard<std::mutex> lock(m_mutex);

    result.add(rect(0, 0, virtual_width, virtual_height, { 0.f, 0.f, 0.f, 0.55f }, 0).get_compiled());
    result.add(rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL, 10).get_compiled());
    result.add(text("Settings", PANEL_X + 24, PANEL_Y + 12, 300, 30, 22, TEXT, overlay_element::left, true).get_compiled());
    result.add(text(m_game_name, PANEL_X + 324, PANEL_Y + 18, PANEL_W - 348, 24, 14, TEXT_DIM, overlay_element::right).get_compiled());

    // the pages, L1/R1
    if (!m_pages.empty()) {
        const uint16_t tab_w = static_cast<uint16_t>((PANEL_W - 48) / m_pages.size());
        for (size_t i = 0; i < m_pages.size(); i++) {
            const int16_t x = static_cast<int16_t>(PANEL_X + 24 + tab_w * i);
            if (i == m_page)
                result.add(rect(x + 4, TABS_Y + 28, tab_w - 8, 3, ACCENT, 1).get_compiled());
            result.add(text(m_pages[i].title, x, TABS_Y, tab_w, 26, 15, i == m_page ? TEXT : TEXT_DIM, overlay_element::center, i == m_page).get_compiled());
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
            if (selected)
                result.add(rect(PANEL_X + 16, y, PANEL_W - 32, ROW_H - 4, SELECTED, 6).get_compiled());
            result.add(text(row.spec.title.empty() ? row_title(row.spec.key) : row.spec.title, PANEL_X + 34, y + 6, 330, 24, 16, TEXT).get_compiled());
            // the value, shortened with "..." to fit its column (a network adapter's long name)
            const std::string arrows_l = row.editable && selected && row.spec.kind != Kind::Bool ? "<  " : "";
            const std::string arrows_r = (arrows_l.empty() ? "" : "  >") + std::string(row.needs_restart ? "  *" : "");
            constexpr uint16_t VALUE_W = PANEL_W - 284;
            std::string value = shown_value(row);
            label value_label = text(arrows_l + value + arrows_r, PANEL_X + 250, y + 6, VALUE_W, 24, 16, row.editable ? TEXT : TEXT_DIM, overlay_element::right);
            uint16_t value_w = 0, value_h = 0;
            value_label.measure_text(value_w, value_h, true);
            while (value_w > VALUE_W && !value.empty()) {
                value.pop_back();
                value_label.set_text(arrows_l + value + "..." + arrows_r);
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
        if (!row.editable)
            help += (help.empty() ? "" : " ") + std::string("Change it in the settings file.");
    }
    // the help gets the space down to the restart note or the hints; a longer one is cut at a word
    const int16_t note_y = FOOTER_Y - 24;
    const uint16_t help_h = static_cast<uint16_t>((any_restart ? note_y - 4 : FOOTER_Y - 6) - HELP_Y);
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
    if (any_restart)
        result.add(text("* takes effect the next time the game starts", PANEL_X + 30, note_y, PANEL_W - 60, 18, 13, ACCENT).get_compiled());

    result.add(text("Up/Down: choose    Left/Right, Cross: change    L1/R1 (Q/E): page    Circle (C), Esc, F1: close",
                   PANEL_X + 24, FOOTER_Y, PANEL_W - 48, 20, 12, TEXT_DIM, overlay_element::center)
            .get_compiled());
    return result;
}

} // namespace game_frontend
