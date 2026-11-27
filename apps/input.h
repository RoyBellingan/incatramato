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

// What the setup screen calls them.
inline constexpr const char* kFnLabels[kFnCount] = {
    "THROTTLE", "BRAKE", "CLUTCH", "SHIFT UP", "SHIFT DOWN", "GEAR 1", "GEAR 2", "GEAR 3", "GEAR 4", "GEAR 5", "GEAR 6",
    "REVERSE", "GEARBOX MODE", "AWD", "DIFF LOCK", "LOW RANGE", "ENGINE BRAKE", "STARTER", "NEXT VEHICLE",
    "LOAD / TIP / BLADE", "PAVE", "ROLL", "RECOVER"};

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
    void open_only(SDL_JoystickID id) {  // tests: just this device, whatever else is plugged in
        close();
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id)
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

    // 0 released .. 1 fully applied, straight from the axis; the game shapes it with each pedal's curve.
    float axis(int fn, const Devices& d) const {
        const Binding& x = b[fn];
        if (x.kind != Binding::Axis || x.full == x.rest) return 0.0f;
        SDL_Joystick* j = d.find(x.device);
        if (!j || x.index >= SDL_JoystickNumAxes(j)) return 0.0f;
        const float t = static_cast<float>(SDL_JoystickGetAxis(j, x.index) - x.rest) / static_cast<float>(x.full - x.rest);
        return std::clamp(t, 0.0f, 1.0f);
    }
    // Pressed: a bound button held, or a bound axis past halfway (an axis can stand in for any button). Not for
    // the pedals: their axis is read as an axis, and treating it as a button too snapped it to full past halfway.
    bool button(int fn, const Devices& d) const {
        const Binding& x = b[fn];
        if (x.kind == Binding::Axis) return !is_pedal(fn) && axis(fn, d) > 0.5f;
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
            if (b[fn].kind == Binding::None) continue;
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

// The setup screen: choose a function, then press the button or move the axis you want for it.
//
// Axes are recognised from the movement itself, not from a picture of the axes taken beforehand: SDL often
// reports 0 for an axis until it first moves, so a pedal resting at one end can look as if it rests in the
// middle. While listening, every axis's lowest and highest values are tracked, with the time each was first
// reached; once one has swept most of its range, the end it reached first is "released" (where it rests) and the
// other "fully applied", and it is bound when it is back at the released end and stays there: let go.
// That also covers pedals wired the other way up and half axes such as a stick pulled back for the brake.
class Setup {
public:
    bool active() const { return active_; }
    bool listening() const { return listening_; }
    int selected() const { return sel_; }
    float travel() const { return travel_; }  // the axis moving most while listening, 0..1 of a full sweep

    void open() {
        active_ = true;
        listening_ = false;
    }
    void close() { active_ = listening_ = false; }
    void move(int delta) {
        if (!listening_) sel_ = (sel_ + delta + kFnCount) % kFnCount;
    }
    void select(int fn) {
        if (!listening_ && fn >= 0 && fn < kFnCount) sel_ = fn;
    }
    void clear(Map& m) {
        if (!listening_) m.b[sel_] = {};
    }
    void stop_listening() { listening_ = false; }

    void listen(const Devices& d) {
        listening_ = true;
        travel_ = 0.0f;
        clock_ = 0.0f;
        sticks_.clear();
        for (SDL_Joystick* j : d.sticks()) {
            Stick st;
            st.joystick = j;
            for (int i = 0; i < SDL_JoystickNumButtons(j); ++i) st.held.push_back(SDL_JoystickGetButton(j, i));  // ignored until let go
            for (int i = 0; i < SDL_JoystickNumAxes(j); ++i) {
                const int v = SDL_JoystickGetAxis(j, i);
                st.axes.push_back({v, v, 0.0f, 0.0f, 0.0f});
            }
            sticks_.push_back(std::move(st));
        }
    }

    // Once per frame while listening. Returns true on the frame the selected function gets its binding.
    bool poll(Map& m, float dt) {
        if (!listening_) return false;
        constexpr int kSweep = 20000;  // of 65535: what counts as moving an axis on purpose
        constexpr int kEnd = 3000;     // this close to an end is at that end
        travel_ = 0.0f;
        clock_ += dt;
        for (Stick& st : sticks_) {
            for (int i = 0; i < static_cast<int>(st.held.size()); ++i) {
                const bool down = SDL_JoystickGetButton(st.joystick, i);
                if (down && !st.held[static_cast<std::size_t>(i)]) return bind(m, {Binding::Button, Devices::name(st.joystick), i, 0, 0});
                st.held[static_cast<std::size_t>(i)] = down;
            }
            for (int i = 0; i < static_cast<int>(st.axes.size()); ++i) {
                Track& a = st.axes[static_cast<std::size_t>(i)];
                const int v = SDL_JoystickGetAxis(st.joystick, i);
                // A new end counts only once well past the old one, so jitter at rest does not move its time.
                if (v < a.lo - kEnd) a.t_lo = clock_;
                if (v > a.hi + kEnd) a.t_hi = clock_;
                a.lo = std::min(a.lo, v), a.hi = std::max(a.hi, v);
                travel_ = std::max(travel_, static_cast<float>(a.hi - a.lo) / 65535.0f);
                if (a.hi - a.lo < kSweep) continue;
                const int rest = a.t_lo <= a.t_hi ? a.lo : a.hi;
                if (std::abs(v - rest) > kEnd) {
                    a.still = 0.0f;
                    continue;
                }
                a.still += dt;
                if (a.still < 0.25f) continue;  // let go, and staying there
                auto snap = [](int x) {
                    for (int anchor : {-32768, 0, 32767})
                        if (std::abs(x - anchor) < kEnd) return anchor;
                    return x;
                };
                return bind(m, {Binding::Axis, Devices::name(st.joystick), i, snap(rest), snap(rest == a.lo ? a.hi : a.lo)});
            }
        }
        return false;
    }

private:
    struct Track {
        int lo, hi;          // lowest and highest readings so far
        float t_lo, t_hi;    // when each was first reached (the reading at the start counts as both, at 0)
        float still;         // how long it has stayed at the released end
    };
    struct Stick {
        SDL_Joystick* joystick = nullptr;
        std::vector<char> held;
        std::vector<Track> axes;
    };
    bool bind(Map& m, Binding x) {
        m.b[sel_] = std::move(x);
        listening_ = false;
        return true;
    }

    bool active_ = false, listening_ = false;
    int sel_ = 0;
    float travel_ = 0.0f, clock_ = 0.0f;
    std::vector<Stick> sticks_;
};

}  // namespace input
