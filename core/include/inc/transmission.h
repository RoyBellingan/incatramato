#pragma once

#include <cstdint>

#include "inc/rig.h"

namespace inc {

enum class ShiftMode : std::uint8_t {
    Manual,      // three pedals and a lever; the driver does everything
    Sequential,  // driver picks the gear with up/down; clutch and rev-matching are automated
    Automatic,   // automated manual: picks the gear too
};

// How the automated boxes shift and pull away. Defaults here; data/tuning.json can change every one.
struct ShiftParams {
    float open_time = 0.10f;      // clutch travel before the gear is pulled [s]
    float max_sync_time = 0.35f;  // longest wait for the revs to match [s]
    float close_time = 0.25f;     // clutch re-engagement [s]
    float min_shift_gap = 1.0f;   // automatic: time between shifts [s]
    // Automatic shift points, as fractions of the redline: up at base + per_throttle * throttle, down likewise.
    float up_base = 0.55f, up_per_throttle = 0.30f;
    float down_base = 0.36f, down_per_throttle = 0.14f;
    // Pulling away: the clutch bites as either side rises from launch_revs * idle over launch_span * idle...
    float launch_revs = 1.1f, launch_span = 0.5f;
    // ...and a touch of throttle bites up to launch_touch of the clutch's capacity, all of it by launch_touch_throttle.
    float launch_touch = 0.15f, launch_touch_throttle = 0.1f;
};

struct DriverInput {
    float throttle = 0.0f;
    float brake = 0.0f;
    bool handbrake = false;
    float clutch = 0.0f;  // the pedal; in Sequential and Automatic it adds to the automated clutch
    int gear = 0;         // Manual: lever position. Automatic: >0 drive, 0 neutral, <0 reverse. Sequential: unused
    int shift = 0;        // Sequential: +1 or -1 on the update the driver asks for a shift
    bool low_range = false;
    bool awd = false;
    bool diff_lock = false;
    bool engine_brake = false;
    bool starter = false;
};

// Turns what the driver does into what the actuators do. In Sequential and Automatic it works the clutch
// for pulling away and stopping, and runs each shift as: clutch out, new gear, match revs, clutch in.
class ShiftController {
public:
    ShiftMode mode = ShiftMode::Automatic;
    ShiftParams params;

    Controls update(const DriverInput& in, const Rig& rig, float dt);

    int selected() const { return target_; }  // the gear it is in or heading for
    bool shifting() const { return phase_ != Phase::Idle; }

private:
    enum class Phase : std::uint8_t { Idle, Open, Sync, Close };
    int target_ = 0;
    Phase phase_ = Phase::Idle;
    float timer_ = 0.0f;
    float since_shift_ = 10.0f;
};

}  // namespace inc
