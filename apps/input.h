#pragma once

// Joysticks, wheels, pedals, throttles and shifters. Everything is bound by device name plus axis or button
// number, so any mix of devices works: nothing here assumes a gamepad layout.
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace input {

enum Fn {
    Throttle, Brake, Clutch, ShiftUp, ShiftDown, Gear1, Gear2, Gear3, Gear4, Gear5, Gear6, Reverse, Mode, Awd, Lock,
    Low, EngineBrake, Starter, NextVehicle, Cargo, Pave, Roll, Recover, kFnCount
};

inline constexpr const char* kFnNames[kFnCount] = {
    "throttle", "brake", "clutch", "shift_up", "shift_down", "gear1", "gear2", "gear3", "gear4", "gear5", "gear6",
    "reverse", "mode", "awd", "diff_lock", "low_range", "engine_brake", "starter", "next_vehicle", "cargo", "pave",
    "roll", "recover"};

inline bool is_pedal(int fn) { return fn == Throttle || fn == Brake || fn == Clutch; }

struct Binding {
    enum Kind { None, Axis, Button } kind = None;
    std::string device;
    int index = 0;
    int rest = 0, full = 0;  // axis: raw value released and fully applied. Covers inverted pedals and half axes.
    bool operator==(const Binding&) const = default;
};

class Devices {
public:
    Devices() = default;
    Devices(const Devices&) = delete;
    Devices& operator=(const Devices&) = delete;
    ~Devices() { close(); }

    void refresh() {  // call at start and whenever a device is plugged or unplugged
        close();
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_Joystick* j = SDL_JoystickOpen(i)) sticks_.push_back(j);
    }
    const std::vector<SDL_Joystick*>& sticks() const { return sticks_; }
    static std::string name(SDL_Joystick* j) {
        const char* n = SDL_JoystickName(j);
        std::string s = n ? n : "unknown";
        std::replace(s.begin(), s.end(), '"', '\'');
        return s;
    }
    SDL_Joystick* find(const std::string& device) const {
        for (SDL_Joystick* j : sticks_)
            if (name(j) == device) return j;
        return nullptr;
    }

private:
    void close() {
        for (SDL_Joystick* j : sticks_) SDL_JoystickClose(j);
        sticks_.clear();
    }
    std::vector<SDL_Joystick*> sticks_;
};

struct Map {
    std::array<Binding, kFnCount> b;

    float axis(int fn, const Devices& d) const {  // 0 released .. 1 fully applied
        const Binding& x = b[fn];
        if (x.kind != Binding::Axis || x.full == x.rest) return 0.0f;
        SDL_Joystick* j = d.find(x.device);
        if (!j || x.index >= SDL_JoystickNumAxes(j)) return 0.0f;
        const float t = static_cast<float>(SDL_JoystickGetAxis(j, x.index) - x.rest) / static_cast<float>(x.full - x.rest);
        return std::clamp((t - 0.04f) / 0.92f, 0.0f, 1.0f);  // small dead band at both ends
    }
    bool button(int fn, const Devices& d) const {
        const Binding& x = b[fn];
        if (x.kind != Binding::Button) return false;
        SDL_Joystick* j = d.find(x.device);
        return j && x.index < SDL_JoystickNumButtons(j) && SDL_JoystickGetButton(j, x.index);
    }
    // H-pattern shifter: each gate holds a button down while the lever is in it. Returns the gate the lever
    // is in (1..6, -1 reverse, 0 neutral), or kNoShifter if no gear is bound to a button.
    static constexpr int kNoShifter = -99;
    int gate(const Devices& d) const {
        bool any = false;
        for (int fn = Gear1; fn <= Reverse; ++fn) {
            if (b[fn].kind != Binding::Button) continue;
            any = true;
            if (button(fn, d)) return fn == Reverse ? -1 : fn - Gear1 + 1;
        }
        return any ? 0 : kNoShifter;
    }

    bool save(const char* path) const {
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f, "# function axis <number> <raw released> <raw applied> \"device\"\n# function button <number> \"device\"\n");
        for (int fn = 0; fn < kFnCount; ++fn) {
            const Binding& x = b[fn];
            if (x.kind == Binding::Axis)
                std::fprintf(f, "%s axis %d %d %d \"%s\"\n", kFnNames[fn], x.index, x.rest, x.full, x.device.c_str());
            else if (x.kind == Binding::Button)
                std::fprintf(f, "%s button %d \"%s\"\n", kFnNames[fn], x.index, x.device.c_str());
        }
        return std::fclose(f) == 0;
    }
    bool load(const char* path) {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        b = {};
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char fn_name[32], kind[16];
            int n0 = 0, n1 = 0, n2 = 0;
            const std::string text = line;
            const std::size_t q0 = text.find('"'), q1 = text.rfind('"');
            if (line[0] == '#' || q0 == std::string::npos || q1 <= q0) continue;
            if (std::sscanf(line, "%31s %15s %d %d %d", fn_name, kind, &n0, &n1, &n2) < 3) continue;
            const auto it = std::find_if(std::begin(kFnNames), std::end(kFnNames),
                                         [&](const char* s) { return std::string(s) == fn_name; });
            if (it == std::end(kFnNames)) continue;
            Binding& x = b[it - std::begin(kFnNames)];
            x.device = text.substr(q0 + 1, q1 - q0 - 1);
            x.index = n0;
            if (std::string(kind) == "axis") {
                x.kind = Binding::Axis;
                x.rest = n1;
                x.full = n2;
            } else {
                x.kind = Binding::Button;
            }
        }
        std::fclose(f);
        return true;
    }
};

// Walks through every function and binds whatever the player moves or presses next.
class Wizard {
public:
    bool active() const { return active_; }
    int current() const { return fn_; }

    void start(const Devices& d) {
        active_ = true;
        fn_ = 0;
        released_ = {};
        arm(d);
    }
    void cancel() { active_ = false; }
    void skip(Map& m, const Devices& d) {
        if (!active_) return;
        m.b[fn_] = {};
        released_ = {};
        advance(d);
    }

    // Call once per frame. Returns true on the frame the last function is done.
    bool poll(Map& m, const Devices& d, float dt) {
        if (!active_) return false;
        if (settle_ > 0.0f) {
            // Wait for the control just bound to be let go, then take a fresh picture of every axis at rest.
            Map probe;
            probe.b[0] = released_;
            const bool held = released_.kind == Binding::Axis ? probe.axis(0, d) > 0.2f : probe.button(0, d);
            if (held) return false;
            settle_ -= dt;
            if (settle_ <= 0.0f) snapshot(d);
            return false;
        }
        for (std::size_t s = 0; s < d.sticks().size(); ++s) {
            SDL_Joystick* j = d.sticks()[s];
            for (int i = 0; i < SDL_JoystickNumButtons(j); ++i)
                if (SDL_JoystickGetButton(j, i)) return bind(m, d, {Binding::Button, Devices::name(j), i, 0, 0});
            if (!is_pedal(fn_)) continue;
            for (int i = 0; i < SDL_JoystickNumAxes(j) && i < static_cast<int>(rest_[s].size()); ++i) {
                const int raw = SDL_JoystickGetAxis(j, i), rest = rest_[s][i];
                if (std::abs(raw - rest) < 24000) continue;
                // Released positions sit at an end or in the middle; snap to it so noise does not shrink the range.
                int snapped = rest;
                for (int anchor : {-32768, 0, 32767})
                    if (std::abs(rest - anchor) < 3000) snapped = anchor;
                return bind(m, d, {Binding::Axis, Devices::name(j), i, snapped, raw > rest ? 32767 : -32768});
            }
        }
        return false;
    }

private:
    bool bind(Map& m, const Devices& d, Binding x) {
        m.b[fn_] = x;
        released_ = std::move(x);
        return advance(d);
    }
    bool advance(const Devices& d) {
        if (++fn_ >= kFnCount) {
            active_ = false;
            return true;
        }
        arm(d);
        return false;
    }
    void arm(const Devices& d) {
        settle_ = 0.3f;
        snapshot(d);
    }
    void snapshot(const Devices& d) {
        rest_.clear();
        for (SDL_Joystick* j : d.sticks()) {
            std::vector<int> axes(SDL_JoystickNumAxes(j));
            for (std::size_t i = 0; i < axes.size(); ++i) axes[i] = SDL_JoystickGetAxis(j, static_cast<int>(i));
            rest_.push_back(std::move(axes));
        }
    }

    bool active_ = false;
    int fn_ = 0;
    float settle_ = 0.0f;
    Binding released_;
    std::vector<std::vector<int>> rest_;
};

}  // namespace input
