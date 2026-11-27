#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace inc {

struct Engine {
    std::vector<std::pair<float, float>> curve;  // (rpm, N*m at full load), ascending rpm
    float idle_rpm;
    float redline_rpm;
    float stall_rpm;       // a running engine dies below this
    float inertia;         // [kg*m^2]
    float friction;        // internal losses at redline [N*m]
    float exhaust_brake;   // extra retarding torque at redline with the exhaust brake on [N*m]
    float starter_torque;  // [N*m]

    struct Torque {
        float net;        // at the flywheel
        float indicated;  // before losses; this is what burns fuel
    };
    // The idle governor is part of the engine: it adds fuel on its own to hold idle_rpm,
    // which is what lets a diesel pull away in a low gear without touching the throttle.
    Torque torque(float rpm, float throttle, bool running, bool exhaust_brake_on) const;
};

// Dry friction clutch. Transmits up to max_torque when fully engaged.
struct Clutch {
    float max_torque;  // [N*m]
};

struct Gearbox {
    std::vector<float> forward;
    std::vector<float> reverse;  // a truck has one; a dozer's powershift has as many as forward, a bit faster
    float final_drive;
    float low_range;

    // gear: 0 neutral, 1..n forward, -1..-m reverse. Returns engine turns per wheel turn (negative in reverse).
    float ratio(int gear, bool low) const;
};

enum class Lock : std::uint8_t { Open, Locked, WithDiffLock, WithAwd };

// The driveline is a tree: leaves drive wheels, inner nodes are differentials (or transfer cases).
struct Shaft {
    int wheel = -1;  // >= 0 for a leaf
    std::vector<Shaft> children;
    Lock lock = Lock::Open;
    bool awd_only = false;  // this branch is disconnected unless AWD is engaged
};

struct DriveState {
    bool awd = false;
    bool diff_lock = false;
};

// Every shaft, seen from above, responds to torque as: acceleration = a * torque + b.
// A wheel alone has a = 1/inertia and b = -(load torque)/inertia. An open differential splits torque equally
// and turns at the mean of its outputs; a locked one forces equal accelerations. Both keep the relation affine,
// so the tree reduces bottom-up and torque is handed out top-down, exactly, for any layout.
struct Affine {
    float a, b;
};

bool connected(const Shaft& s, DriveState st);
Affine reduce(const Shaft& s, DriveState st, std::span<const float> a, std::span<const float> b);
void distribute(const Shaft& s, DriveState st, float torque, std::span<const float> a, std::span<const float> b,
                std::span<float> wheel_torque);
float shaft_speed(const Shaft& s, DriveState st, std::span<const float> omega);
// Makes the outputs of every locked differential turn at the same speed (needed when a lock is engaged).
void enforce_locks(const Shaft& s, DriveState st, std::span<float> omega);

}  // namespace inc
