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

#pragma once

// The in-game settings menu: an overlay over the paused game, built from the game's settings
// declarations (settings.h), one page per section. It only edits its own copy of the values; the
// changes go to the front end's main thread (take_changes), which writes the settings file and
// applies the live ones (frontend.cpp). Its Controls page holds the bindings ([keyboard], [gamepad],
// [touch]: each Vita button's two keys and two gamepad buttons, each touch's), set by pressing the new key
// or button. Its last page, Exit, quits the game (exit_requested).

#include "settings.h"

#include <overlay/user_interface.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace game_frontend {

class SettingsMenu : public ::overlay::user_interface {
public:
    struct Change {
        std::string section, key, value;
    };
    // what a Controls page row binds: a Vita button ([keyboard] and [gamepad]) or a touch ([touch])
    enum class Binding { None, Button, Touch };
    struct Row {
        std::string android_action;
        std::string section;
        SettingSpec spec;
        std::string value;
        bool editable = true;
        bool needs_restart = false; // changed, and takes effect at the next start
        bool exit = false; // the Exit page's row: Cross twice quits the game
        // the Controls page
        Binding binding = Binding::None;
        bool header = false; // the column names above its bindings (never selected)
        bool reset = false; // Reset all: Cross twice puts every binding back to its default
        std::string name; // [keyboard] / [gamepad] key, or the touch's name
        std::vector<std::string> keys, default_keys; // up to two, as key_name writes them
        std::vector<std::string> gamepads, default_gamepads; // up to two, as [gamepad] names them (a trigger too)
        std::string fixed_gamepad; // what drives it on a gamepad when that can't be changed (a stick)
    };
    struct Page {
        std::string title;
        std::vector<Row> rows;
    };

    // touches, default_touches: [touch] as read, and the game's defaults (Profile::default_touches)
    SettingsMenu(const IniLayout &layout, const Values &values, const std::string &game_name,
        const std::vector<TouchBinding> &touches = {}, const std::vector<TouchBinding> &default_touches = {});

    ::overlay::compiled_resource get_compiled() override;
    void on_button_pressed(::overlay::pad_button button, bool is_auto_repeat) override;

    // the changes since the last call, in order (main thread)
    std::vector<Change> take_changes();
    // Circle (or the front end's F1 / Esc): the menu wants to close
    bool close_requested() const { return m_close_requested.load(); }
    void request_close();
    // Exit the game, confirmed: the front end quits as when the window is closed
    bool exit_requested() const { return m_exit_requested.load(); }

    // a text_entry row being edited (Cross on it): the front end then sends the keyboard here instead
    // of to the game, and turns on text input (Android's on-screen keyboard)
    bool text_editing() const { return m_text_editing.load(); }
    enum class TextKey { Backspace, Delete, Left, Right, Home, End, Enter, Escape };
    void text_input(const std::string &text); // typed characters (UTF-8; printable ASCII is kept)
    void text_key(TextKey key);

    // A Controls page binding waiting for its new key or button (Cross on it): the front end then sends
    // the raw input here instead of to the game (main thread). A key cell takes a key or a mouse button
    // (Escape, or a gamepad button, cancels); a gamepad cell a gamepad button or a trigger (Escape or
    // any key cancels). It gives up after a few seconds.
    bool binding_wait() const { return m_listening.load(); }
    void bind_key(int scancode);
    void bind_mouse(int sdl_button);
    void bind_gamepad_button(int sdl_button);
    void bind_gamepad_trigger(int sdl_axis);

private:
    std::mutex m_mutex; // the pages and the selection (input thread and render thread)
    std::vector<Page> m_pages;
    std::string m_game_name;
    size_t m_page = 0, m_row = 0, m_scroll = 0;
    std::vector<Change> m_changes;
    std::atomic<bool> m_close_requested{ false };
    std::atomic<bool> m_exit_requested{ false };
    bool m_exit_armed = false; // Cross pressed once on Exit the game; moving away disarms it
    std::atomic<bool> m_text_editing{ false };
    std::string m_edit_text; // the selected row's text while it is edited
    size_t m_edit_cursor = 0; // 0 to its length (the end: a new character)
    // the Controls page: the selected column (0, 1: the keys, 2, 3: the gamepad buttons), whether a binding
    // waits for its input, since when, and until when the menu's own buttons are ignored (the press that
    // was bound still reaching the menu)
    size_t m_column = 0;
    std::atomic<bool> m_listening{ false };
    std::chrono::steady_clock::time_point m_listen_start{}, m_ignore_until{};
    bool m_reset_armed = false;
    ::overlay::pad_button m_swallowed = ::overlay::pad_button::pad_button_max_enum; // its repeats are ignored too
    std::string m_note; // what the last binding did (a swap), under the help

    void step(Row &row, int direction);
    void binding_button(Row &row, ::overlay::pad_button button);
    void end_listen();
    void listen_timeout();
    bool listen_ready() const; // past the first moments of listening (the press that started it)
    void set_keys(Row &row, std::vector<std::string> keys);
    void set_gamepads(Row &row, std::vector<std::string> gamepads);
    void assign_key(Row &row, size_t column, const std::string &key);
    void assign_gamepad(Row &row, size_t slot, const std::string &gamepad);
    void reset_bindings();
    std::vector<Row *> binding_rows();
    Row *selected_row();
    void end_edit(bool commit);
    void edit_button(::overlay::pad_button button);
    void move_row(int direction);
    void move_page(int direction);
};

} // namespace game_frontend
