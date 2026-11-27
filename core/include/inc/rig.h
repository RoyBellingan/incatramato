#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "inc/drivetrain.h"
#include "inc/soil.h"
#include "inc/terrain.h"

namespace inc {

struct WheelSpec {
    float x, y;           // position relative to the vehicle origin; x forward [m]
    float load_fraction;  // share of the vehicle weight carried
    bool handbrake = false;  // the parking brake acts on this wheel
    float payload_fraction = -1.0f;  // share of the payload carried; negative: same as load_fraction
};

struct VehicleSpec {
    std::string name;
    float mass;           // [kg]
    float wheel_radius;   // wheel or sprocket [m]
    float wheel_inertia;  // [kg*m^2]
    float clearance;      // sinkage at which the chassis starts dragging [m]
    float brake_torque;   // per wheel at full brake [N*m]
    std::vector<WheelSpec> wheels;
    Engine engine;
    Clutch clutch;
    Gearbox gearbox;
    Shaft driveline;
    Tyre tyre;
};

// What the vehicle's actuators are doing. A human with three pedals and a lever produces this directly;
// ShiftController (transmission.h) produces it for sequential and automatic boxes.
struct Controls {
    float throttle = 0.0f;
    float brake = 0.0f;
    bool handbrake = false;  // full braking on the wheels marked handbrake, on top of the pedal
    float clutch = 0.0f;  // pedal: 0 released (engaged), 1 on the floor (disengaged)
    int gear = 0;         // lever: 0 neutral, 1..n forward, negative reverse
    bool low_range = false;
    bool awd = false;
    bool diff_lock = false;
    bool engine_brake = false;
    bool starter = false;
};

// What each wheel is doing at the end of the last step. All forces along the direction of travel.
struct WheelTelemetry {
    float load;          // vertical load [N] (fixed share of the weight until suspension exists)
    float sinkage;       // equilibrium sinkage into the soft layer [m]
    float rut;           // rut depth under the wheel [m]
    float gravel;        // gravel under the wheel [m]
    float softness;      // 1 on mud, 0 on hard surfaces
    float slip;          // slip ratio: (wheel surface speed - ground speed) / reference speed
    float traction;      // ground force on the wheel [N]
    float max_traction;  // what the ground can give at full slip [N]
    float resistance;    // rolling plus compaction resistance [N]
    float drive_torque;  // from the driveline [N*m]
    float brake;         // how hard it is braked, 0..1
    float omega;         // [rad/s]
};

struct BodyTelemetry {
    float traction = 0.0f;       // sum over the wheels [N]
    float resistance = 0.0f;     // rolling and compaction [N]
    float grade = 0.0f;          // weight component along the slope, positive downhill-forward [N]
    float belly = 0.0f;          // chassis dragging when high-centred [N]
    float external = 0.0f;       // set_external_resistance, e.g. a loaded blade [N]
    float engine_torque = 0.0f;  // at the flywheel, starter included [N*m]
    float clutch_torque = 0.0f;  // [N*m]
    float clutch_capacity = 0.0f;
    float clutch_slip_rpm = 0.0f;
    float fuel_rate = 0.0f;  // [kg/s]
};

// Straight-line test rig: the full powertrain and ground model, with the body reduced to one degree of
// freedom along x and fixed wheel loads. It exists to test and tune the core without a physics engine.
// In the game the rigid body, suspension, steering and lateral forces belong to the engine's physics.
class Rig {
public:
    Rig(VehicleSpec spec, Terrain& terrain, SoilParams soil, float x, float y);

    void step(const Controls& u, float dt);
    // A passive force against the motion from something the vehicle drags or pushes (a blade, a sledge).
    void set_external_resistance(float newtons) { external_ = newtons; }
    // Cargo carried [kg]: adds to the weight on the wheels (by payload_fraction) and to the mass to accelerate.
    void set_payload(float kg) { payload_ = std::max(0.0f, kg); }
    float payload() const { return payload_; }
    float mass() const { return spec_.mass + payload_; }

    std::span<const WheelTelemetry> wheel_telemetry() const { return wheel_tm_; }
    const BodyTelemetry& telemetry() const { return body_tm_; }

    const VehicleSpec& spec() const { return spec_; }
    float x() const { return x_; }
    float y() const { return y_; }
    float speed() const { return v_; }
    float fuel_kg() const { return fuel_; }
    float mean_sinkage() const { return sinkage_; }  // how far the wheels sit below the ground between them
    std::span<const float> wheel_speeds() const { return omega_; }
    std::span<const float> wheel_torques() const { return torque_; }

    float engine_rpm() const;
    bool engine_running() const { return running_; }
    int gear() const { return gear_; }  // the gear actually engaged
    bool low_range() const { return low_; }
    bool clutch_locked() const { return clutch_locked_; }
    bool grinding() const { return grinding_; }  // the lever was pushed at a gear that would not go in
    float input_rpm(int gear) const;             // speed the gearbox input would turn at in that gear

private:
    VehicleSpec spec_;
    Terrain* terrain_;
    SoilParams soil_;
    float x_, y_;
    float v_ = 0.0f;
    float fuel_ = 0.0f;
    float sinkage_ = 0.0f;
    float engine_omega_;
    float shaft_ = 0.0f;
    int gear_ = 0;
    bool low_ = false;
    bool running_ = true;
    bool clutch_locked_ = false;
    bool grinding_ = false;
    std::vector<float> omega_, torque_, a_, b_, f_, df_, fmax_;
    std::vector<char> diggable_;
    float external_ = 0.0f;
    float payload_ = 0.0f;
    std::vector<WheelTelemetry> wheel_tm_;
    BodyTelemetry body_tm_;
};

// Minimal driver for tests and the demo: automatic box, throttle towards a target speed.
struct Driver {
    float target_speed = 10.0f;  // [m/s]
    bool low_range = false;
    bool awd = false;
    bool diff_lock = false;
};

struct RunResult {
    bool reached;   // false: stuck or timed out
    float time;     // [s]
    float x;        // final position [m]
    float fuel_kg;  // burnt during this run
};

RunResult drive_to(Rig& rig, const Driver& driver, float goal_x, float timeout = 600.0f, float dt = 1e-3f);

}  // namespace inc
