#pragma once

#include <cstdint>

#include "inc/rig.h"

namespace inc {

enum class ShiftMode : std::uint8_t {
    Manual,      // three pedals and a lever; the driver does everything
    Sequential,  // driver picks the gear with up/down; clutch and rev-matching are automated
    Automatic,   // automated manual: picks the gear too
};

struct DriverInput {
    float throttle = 0.0f;
    float brake = 0.0f;
    bool handbrake = false;
    float clutch = 0.0f;  // Manual only
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
