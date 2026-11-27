#pragma once

// Every number worth tuning, in one place: the built-in defaults, which data/tuning.json can override. Loading a
// file starts from these defaults, so a partial file is fine and anything missing keeps its default.

#include <map>
#include <string>
#include <vector>

#include "inc/presets.h"
#include "inc/transmission.h"

namespace inc {

// Defaults of the site rules (see Rules).
inline constexpr float kLoad = 10.0f;            // gravel per truck load at the quarry [m3]
inline constexpr float kMinBase = 0.15f;         // gravel needed under asphalt [m]
inline constexpr float kTipAhead = 6.2f;         // tipped gravel lands this far ahead; nothing turns round in one lane [m]
inline constexpr float kTipRate = 1.5f;          // gravel out of the bed while tipping [m3/s]
inline constexpr float kTipWidth = 2.2f;         // the load pours over the tailgate along this width [m]
inline constexpr float kTipClearance = 3.0f;     // no tipping this close to another vehicle: a full heap spreads ~2.4 m [m]
inline constexpr float kBladeAhead = 2.05f;      // cutting edge ahead of the dozer's centre [m]
inline constexpr float kBladeWidth = 3.4f;       // [m]
inline constexpr float kGravelDensity = 1800.0f; // loose aggregate [kg/m3]
inline constexpr float kPushFriction = 0.5f;     // force to shove a heap, per unit of its weight

// The rules of the site: loading, tipping, the blade, gravel.
struct Rules {
    float load = kLoad, min_base = kMinBase;
    float tip_ahead = kTipAhead, tip_rate = kTipRate, tip_width = kTipWidth, tip_clearance = kTipClearance;
    float blade_ahead = kBladeAhead, blade_width = kBladeWidth;
    float gravel_density = kGravelDensity, push_friction = kPushFriction;
    float settle_time = 2.0f;  // a fresh heap keeps slumping this long [s]
};

// How a pedal axis turns into a pedal position: nothing below the dead zone, then position ^ gamma (1 straight,
// above 1 gentler at first, below 1 sharper). Read by the playground, not by the simulation.
struct PedalCurve {
    float deadzone = 0.04f;
    float gamma = 1.0f;
};
struct Pedals {
    PedalCurve throttle, brake, clutch;
    // Keyboard pedals ramp at these rates [full travel per second].
    float key_throttle_up = 3.0f, key_throttle_down = 6.0f;
    float key_brake_up = 6.0f, key_brake_down = 8.0f;
    float key_clutch_down = 10.0f, key_clutch_up = 1.6f;
};

// Driving aids, as a new machine starts with them; the driver can switch each one in the cab.
struct Assists {
    bool blade_grade_control = true;  // the dozer holds its blade layer itself (BladeMode::Grade)
};

struct Tuning {
    SoilParams soil;
    ShiftParams shift;
    Rules rules;
    Assists assists;
    Pedals pedals;
    std::map<std::string, VehicleSpec> vehicles;  // without their tyres: a vehicle is fitted with one by name
    std::map<std::string, Tyre> tyres;

    static Tuning defaults();
    VehicleSpec spec(const std::string& vehicle, const std::string& tyre) const;  // the vehicle fitted with that tyre
};

struct TuningResult {
    bool ok = true;
    std::string error;                  // when not ok: what and where (file:line:column)
    std::vector<std::string> warnings;  // unknown keys, wrong types: probably typos, ignored
};

// Overrides `t` with whatever the JSON says. On a syntax error nothing is changed. // comments and trailing
// commas are allowed.
TuningResult load_tuning(const std::string& path, Tuning& t);
TuningResult load_tuning_text(const std::string& text, Tuning& t, const std::string& name = "tuning");

}  // namespace inc
