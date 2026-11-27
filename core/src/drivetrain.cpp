#include "inc/drivetrain.h"

#include <algorithm>

namespace inc {
namespace {

bool is_locked(const Shaft& s, DriveState st) {
    switch (s.lock) {
        case Lock::Locked: return true;
        case Lock::WithDiffLock: return st.diff_lock;
        case Lock::WithAwd: return st.awd;
        case Lock::Open: break;
    }
    return false;
}

void shift(const Shaft& s, DriveState st, float delta, std::span<float> omega) {
    if (s.wheel >= 0) {
        omega[s.wheel] += delta;
        return;
    }
    for (const Shaft& c : s.children)
        if (connected(c, st)) shift(c, st, delta, omega);
}

}  // namespace

Engine::Torque Engine::torque(float rpm, float throttle, bool running, bool exhaust_brake_on) const {
    rpm = std::max(rpm, 0.0f);
    const float loss = rpm > 1.0f ? friction * (0.3f + 0.7f * rpm / redline_rpm) : 0.0f;
    if (!running) return {-loss, 0.0f};

    float full = 0.0f;
    if (rpm <= curve.front().first) {
        full = curve.front().second * rpm / curve.front().first;
    } else if (rpm < curve.back().first) {
        const auto hi = std::upper_bound(curve.begin(), curve.end(), rpm,
                                         [](float v, const std::pair<float, float>& p) { return v < p.first; });
        const auto lo = hi - 1;
        full = lo->second + (hi->second - lo->second) * (rpm - lo->first) / (hi->first - lo->first);
    } else {
        full = curve.back().second;
    }
    // Governor: fade the torque out over the last 2% above the redline.
    full *= std::clamp((redline_rpm * 1.02f - rpm) / (redline_rpm * 0.02f), 0.0f, 1.0f);

    // The throttle adds to whatever the governor is already giving, rather than competing with it: with
    // max(throttle, governor) the first fifth of the pedal did nothing at idle. Above idle the governor gives
    // nothing and the throttle is the demand, as before.
    const float governor = std::clamp((idle_rpm - rpm) / governor_band, 0.0f, 1.0f);
    const float demand = governor + std::clamp(throttle, 0.0f, 1.0f) * (1.0f - governor);
    Torque t{demand * full - loss, demand * full};
    if (exhaust_brake_on && throttle < 0.02f && rpm > idle_rpm + 150.0f) t.net -= exhaust_brake * rpm / redline_rpm;
    return t;
}

float Gearbox::ratio(int gear, bool low) const {
    if (gear == 0 || (gear < 0 ? reverse.empty() : forward.empty())) return 0.0f;  // no such gear: neutral
    const float g = gear < 0 ? -reverse[std::min<std::size_t>(-gear, reverse.size()) - 1]
                             : forward[std::min<std::size_t>(gear, forward.size()) - 1];
    return g * final_drive * (low ? low_range : 1.0f);
}

bool connected(const Shaft& s, DriveState st) {
    if (s.awd_only && !st.awd) return false;
    if (s.wheel >= 0) return true;
    return std::any_of(s.children.begin(), s.children.end(), [&](const Shaft& c) { return connected(c, st); });
}

Affine reduce(const Shaft& s, DriveState st, std::span<const float> a, std::span<const float> b) {
    if (s.wheel >= 0) return {a[s.wheel], b[s.wheel]};
    float n = 0.0f, sum_a = 0.0f, sum_b = 0.0f, sum_inv_a = 0.0f, sum_b_over_a = 0.0f;
    for (const Shaft& c : s.children) {
        if (!connected(c, st)) continue;
        const Affine r = reduce(c, st, a, b);
        n += 1.0f;
        sum_a += r.a;
        sum_b += r.b;
        sum_inv_a += 1.0f / r.a;
        sum_b_over_a += r.b / r.a;
    }
    if (is_locked(s, st)) return {1.0f / sum_inv_a, sum_b_over_a / sum_inv_a};
    return {sum_a / (n * n), sum_b / n};
}

void distribute(const Shaft& s, DriveState st, float torque, std::span<const float> a, std::span<const float> b,
                std::span<float> wheel_torque) {
    if (s.wheel >= 0) {
        wheel_torque[s.wheel] = torque;
        return;
    }
    const bool locked = is_locked(s, st);
    const Affine self = reduce(s, st, a, b);
    const float accel = self.a * torque + self.b;
    const auto n = static_cast<float>(
        std::count_if(s.children.begin(), s.children.end(), [&](const Shaft& c) { return connected(c, st); }));
    for (const Shaft& c : s.children) {
        if (!connected(c, st)) continue;
        if (locked) {
            const Affine r = reduce(c, st, a, b);
            distribute(c, st, (accel - r.b) / r.a, a, b, wheel_torque);
        } else {
            distribute(c, st, torque / n, a, b, wheel_torque);
        }
    }
}

float shaft_speed(const Shaft& s, DriveState st, std::span<const float> omega) {
    if (s.wheel >= 0) return omega[s.wheel];
    float n = 0.0f, sum = 0.0f;
    for (const Shaft& c : s.children) {
        if (!connected(c, st)) continue;
        sum += shaft_speed(c, st, omega);
        n += 1.0f;
    }
    return n > 0.0f ? sum / n : 0.0f;
}

void enforce_locks(const Shaft& s, DriveState st, std::span<float> omega) {
    if (s.wheel >= 0) return;
    for (const Shaft& c : s.children)
        if (connected(c, st)) enforce_locks(c, st, omega);
    if (!is_locked(s, st)) return;
    const float target = shaft_speed(s, st, omega);
    for (const Shaft& c : s.children)
        if (connected(c, st)) shift(c, st, target - shaft_speed(c, st, omega), omega);
}

}  // namespace inc
