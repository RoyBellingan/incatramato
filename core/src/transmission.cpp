#include "inc/transmission.h"

#include <algorithm>
#include <cmath>

namespace inc {
namespace {

constexpr float kRadToRpm = 60.0f / (2.0f * 3.14159265f);

}  // namespace

Controls ShiftController::update(const DriverInput& in, const Rig& rig, float dt) {
    Controls u;
    u.throttle = in.throttle;
    u.brake = in.brake;
    u.handbrake = in.handbrake;
    u.low_range = in.low_range;
    u.awd = in.awd;
    u.diff_lock = in.diff_lock;
    u.engine_brake = in.engine_brake;
    u.starter = in.starter;

    if (mode == ShiftMode::Manual) {
        u.clutch = in.clutch;
        u.gear = target_ = in.gear;
        phase_ = Phase::Idle;
        return u;
    }

    const VehicleSpec& spec = rig.spec();
    const Engine& engine = spec.engine;
    const int top = static_cast<int>(spec.gearbox.forward.size());
    const int top_reverse = static_cast<int>(spec.gearbox.reverse.size());
    const float rpm = rig.engine_rpm();
    const bool stopped = std::abs(rig.speed()) < 0.5f;
    since_shift_ += dt;

    const int dir = in.gear > 0 ? 1 : -1;  // Automatic: the direction asked for
    if (mode == ShiftMode::Sequential) {
        // Up is a higher gear in either direction: from N, down is R1; in reverse, up is R2, R3 and down goes
        // back towards N. A gear against the direction of travel waits in N until the vehicle stops.
        if (in.shift != 0) {
            int next = target_ > 0 || (target_ == 0 && in.shift > 0) ? std::clamp(target_ + in.shift, 0, top)
                                                                     : std::clamp(target_ - in.shift, -top_reverse, 0);
            if (target_ == 0 && in.shift < 0) next = top_reverse > 0 ? -1 : 0;
            const int rolling = stopped ? 0 : (rig.speed() > 0.0f ? 1 : -1);
            if (next != 0 && rolling != 0 && (next > 0 ? 1 : -1) != rolling) next = 0;  // no reverse at speed: stay in N
            target_ = next;
        }
    } else if (in.gear == 0) {
        target_ = 0;
    } else if (target_ * dir <= 0) {  // neutral, or the other direction: change over only when stopped
        if (target_ == 0 || stopped) target_ = dir;
    } else if (phase_ == Phase::Idle && since_shift_ > params.min_shift_gap) {
        // Shift points move up with the throttle. Upshifts are judged on road speed, so wheelspin in mud
        // does not trigger them; downshifts need the engine to be lugging as well as the road speed to be low.
        // Reverse works the same way through its own gears (a dozer has three).
        auto road_rpm = [&](int g) {
            return std::abs(rig.speed() / spec.wheel_radius * spec.gearbox.ratio(g, rig.low_range())) * kRadToRpm;
        };
        const int n = std::abs(target_), highest = dir > 0 ? top : top_reverse;
        const float up = engine.redline_rpm * (params.up_base + params.up_per_throttle * in.throttle);
        const float down = engine.redline_rpm * (params.down_base + params.down_per_throttle * in.throttle);
        if (rpm > up && n < highest && road_rpm(dir * (n + 1)) > 1.1f * down) target_ += dir;
        else if (n > 1 && road_rpm(target_) < down && rpm < 1.15f * down) target_ -= dir;
    }

    // Pulling away and stopping: the clutch bites as either side of it rises above idle. A touch of throttle
    // also starts it biting, gently (at most 15% of its capacity, which an idling engine can hold): otherwise
    // small throttle never raised the revs enough and the truck sat still until a fifth of the pedal.
    float pedal = 0.0f;
    if (target_ != 0 && !(rig.clutch_locked() && rpm > 1.08f * engine.idle_rpm)) {
        const float fastest = std::max(rpm, std::abs(rig.input_rpm(target_)));
        const float revs =
            std::clamp((fastest - params.launch_revs * engine.idle_rpm) / (params.launch_span * engine.idle_rpm), 0.0f, 1.0f);
        const float asked =
            in.throttle > 0.01f ? params.launch_touch * std::clamp(in.throttle / params.launch_touch_throttle, 0.0f, 1.0f) : 0.0f;
        const float bite = std::max(revs, asked);
        pedal = bite >= 1.0f ? 0.0f : 0.8f - 0.6f * bite;
    }

    if (phase_ == Phase::Idle && rig.gear() != target_) {
        phase_ = Phase::Open;
        timer_ = 0.0f;
    }
    timer_ += dt;
    switch (phase_) {
        case Phase::Idle:
            u.gear = target_;
            u.clutch = pedal;
            break;
        case Phase::Open:
            u.gear = rig.gear();
            u.clutch = 1.0f;
            u.throttle = 0.0f;
            if (timer_ >= params.open_time) {
                phase_ = Phase::Sync;
                timer_ = 0.0f;
            }
            break;
        case Phase::Sync: {
            // Blip or lift so the engine meets the new gear at the right speed.
            const float want = std::max(engine.idle_rpm, std::abs(rig.input_rpm(target_)));
            u.gear = target_;
            u.clutch = 1.0f;
            u.throttle = target_ == 0 ? 0.0f : std::clamp((want - rpm) / 300.0f, 0.0f, 1.0f);
            if (rig.gear() == target_ && (std::abs(rpm - want) < 100.0f || timer_ >= params.max_sync_time)) {
                phase_ = Phase::Close;
                timer_ = 0.0f;
                since_shift_ = 0.0f;
            }
            break;
        }
        case Phase::Close: {
            const float k = std::min(timer_ / params.close_time, 1.0f);
            u.gear = target_;
            u.clutch = std::max(pedal, 1.0f - k);
            u.throttle = in.throttle * k;
            if (k >= 1.0f) phase_ = Phase::Idle;
            break;
        }
    }
    // A driver with a clutch pedal can still use it: it disengages at least as far as they press it.
    u.clutch = std::max(u.clutch, in.clutch);
    return u;
}

}  // namespace inc
