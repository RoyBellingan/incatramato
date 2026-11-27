#include "inc/rig.h"

#include "inc/transmission.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace inc {
namespace {

constexpr float kG = 9.81f;
constexpr float kRadToRpm = 60.0f / (2.0f * 3.14159265f);
constexpr float kBsfc = 210e-3f / 3.6e6f;  // kg of diesel per joule at the flywheel (210 g/kWh)
constexpr float kMinSlipSpeed = 0.3f;      // keeps the slip ratio finite near standstill [m/s]
constexpr float kBellyFriction = 0.6f;
constexpr float kSyncWindow = 15.0f;  // a gear goes in without the clutch if the speeds match this closely [rad/s]
constexpr float kLockSpeed = 1.0f;    // clutch slip below which it can lock [rad/s]
constexpr float kBrakeBand = 0.02f;   // wheel speed below which brake torque fades out [rad/s]

float sign(float v) { return v < 0.0f ? -1.0f : 1.0f; }

// The ground a contact patch stands on: the cells under it, averaged. A 3 m track bridges a hole a tyre has
// dug instead of dropping into it; a tyre's patch is a cell or two, so for tyres this is close to the cell
// under the centre. Sampled once per cell length across and along the patch.
Cell footprint(const Terrain& t, float x, float y, float width, float length) {
    const float cs = t.cell_size();
    const int nx = std::max(1, static_cast<int>(std::lround(length / cs)));
    const int ny = std::max(1, static_cast<int>(std::lround(width / cs)));
    double hard = 0.0, soft = 0.0, gravel = 0.0, rut = 0.0, loose = 0.0;
    int paved[3] = {0, 0, 0};
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const Cell& c = t.at(x + length * ((i + 0.5f) / nx - 0.5f), y + width * ((j + 0.5f) / ny - 0.5f));
            hard += c.hard, soft += c.soft, gravel += c.gravel, rut += c.rut, loose += c.loose;
            ++paved[static_cast<int>(c.paving)];
        }
    const double n = static_cast<double>(nx) * ny;
    Cell out;
    out.hard = static_cast<float>(hard / n);
    out.soft = static_cast<float>(soft / n);
    out.gravel = static_cast<float>(gravel / n);
    out.rut = static_cast<float>(rut / n);
    out.paving = static_cast<Paving>(std::max_element(paved, paved + 3) - paved);  // what most of the patch is
    out.loose = out.paving == Paving::None ? 0.0f : static_cast<float>(loose / n);
    if (out.paving == Paving::None) out.gravel += static_cast<float>(loose / n);
    return out;
}

// How hard a wheel is braked, 0..1: the pedal, or full with the handbrake on a wheel it acts on.
float braking(const Controls& u, const WheelSpec& w) { return u.handbrake && w.handbrake ? 1.0f : u.brake; }

}  // namespace

Rig::Rig(VehicleSpec spec, Terrain& terrain, SoilParams soil, float x, float y)
    : spec_(std::move(spec)), terrain_(&terrain), soil_(soil), x_(x), y_(y), engine_omega_(spec_.engine.idle_rpm / kRadToRpm) {
    const std::size_t n = spec_.wheels.size();
    for (auto* v : {&omega_, &torque_, &a_, &b_, &f_, &df_, &fmax_}) v->assign(n, 0.0f);
    diggable_.assign(n, 0);
    wheel_tm_.assign(n, WheelTelemetry{});
    for (const WheelSpec& w : spec_.wheels)  // the wheel tracks: one per distinct lateral position
        if (std::find(track_y_.begin(), track_y_.end(), w.y) == track_y_.end()) track_y_.push_back(w.y);
}

void Rig::step(const Controls& u, float dt) {
    const std::size_t n = spec_.wheels.size();
    const Tyre& tyre = spec_.tyre;
    const float r = spec_.wheel_radius;
    const Engine& engine = spec_.engine;
    const DriveState drive{u.awd, u.diff_lock};
    const bool has_drive = connected(spec_.driveline, drive);

    enforce_locks(spec_.driveline, drive, omega_);
    shaft_ = has_drive ? shaft_speed(spec_.driveline, drive, omega_) : 0.0f;

    // Lever and range selector. A gear goes in with the clutch down, or without it if the speeds match;
    // otherwise it grinds and the box is left in neutral.
    const float bite = std::clamp((0.8f - u.clutch) / 0.6f, 0.0f, 1.0f);  // the pedal bites between 20% and 80%
    if (gear_ == 0 || bite <= 0.0f || std::abs(v_) < 0.5f) low_ = u.low_range;
    grinding_ = false;
    if (u.gear != gear_) {
        const float input = shaft_ * spec_.gearbox.ratio(u.gear, low_);
        if (u.gear == 0 || bite <= 0.0f || std::abs(engine_omega_ - input) < kSyncWindow) {
            gear_ = u.gear;
        } else {
            gear_ = 0;
            grinding_ = true;
        }
    }
    const float ratio = spec_.gearbox.ratio(gear_, low_);
    const float heading = std::abs(v_) > 1e-3f ? sign(v_) : sign(ratio);

    // Ground: sink, measure what resists motion, and linearise each wheel's traction around its current speed.
    float resist = 0.0f, sink = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const WheelSpec& w = spec_.wheels[i];
        const float wx = x_ + w.x, wy = y_ + w.y;
        const float share = w.payload_fraction < 0.0f ? w.load_fraction : w.payload_fraction;
        const float load = spec_.mass * kG * w.load_fraction + payload_ * kG * share;

        terrain_->press(wx, wy, tyre.width, tyre.length,
                        equilibrium_sinkage(footprint(*terrain_, wx, wy, tyre.width, tyre.length), soil_, tyre, load));
        const Cell under = footprint(*terrain_, wx, wy, tyre.width, tyre.length);
        const Cell& ahead = terrain_->at(wx + heading * 0.5f * (tyre.length + terrain_->cell_size()), wy);
        const Contact c = contact(under, soil_, tyre, load);
        const float wheel_resist = (compaction_resistance(ahead, soil_, tyre, load) + c.rolling) *
                                   (1.0f + soil_.viscous * c.softness * std::abs(v_));
        resist += wheel_resist;
        // How far the wheel sits below the crown between the tracks. Only fixed ground counts (firm, mud, the road):
        // loose gravel between the wheels is pushed aside by the axles, it does not hold the chassis up.
        sink += std::max(0.0f, terrain_->at(wx, y_).fixed_top() - under.surface());

        const float slip_v = omega_[i] * r - v_;
        const float denom = std::max({std::abs(omega_[i] * r), std::abs(v_), kMinSlipSpeed});
        const float e = std::exp(-std::abs(slip_v) / denom * c.slip_gain);
        fmax_[i] = c.max_traction;
        f_[i] = sign(slip_v) * c.max_traction * (1.0f - e);
        df_[i] = c.max_traction * c.slip_gain * e * r / denom;  // d(traction)/d(omega)
        diggable_[i] = c.diggable;
        WheelTelemetry& tm = wheel_tm_[i];
        tm.load = load;
        tm.sinkage = equilibrium_sinkage(under, soil_, tyre, load);
        tm.rut = under.rut;
        tm.gravel = under.gravel;
        tm.softness = c.softness;
        tm.slip = slip_v / denom;
        tm.max_traction = c.max_traction;
        tm.resistance = wheel_resist;
        tm.brake = braking(u, w);

        // Semi-implicit: traction is stiff in omega, so fold its slope into the inertia.
        // Brakes: full torque against the rotation, fading to zero only inside a narrow band around standstill so
        // they never push a wheel backwards. Inside the band the torque is stiff in omega, so like the traction
        // slope it is folded into the inertia (implicit), which lets the band be narrow enough to hold a vehicle
        // on a slope: a 14 t truck on 10% creeps about a millimetre a second on the handbrake.
        const float capacity = braking(u, w) * spec_.brake_torque;
        const bool in_band = std::abs(omega_[i]) < kBrakeBand;
        const float inertia = spec_.wheel_inertia + dt * df_[i] * r + (in_band ? dt * capacity / kBrakeBand : 0.0f);
        const float brake = capacity * std::clamp(omega_[i] / kBrakeBand, -1.0f, 1.0f);
        a_[i] = 1.0f / inertia;
        b_[i] = -(f_[i] * r + brake) / inertia;
    }
    sinkage_ = sink / static_cast<float>(n);

    // Engine, clutch and driveline. The clutch either holds (both sides turn together and it carries whatever
    // torque that takes) or slips (it carries its full capacity, towards closing the speed difference).
    const Engine::Torque et = engine.torque(engine_omega_ * kRadToRpm, u.throttle, running_, u.engine_brake);
    float engine_torque = et.net;
    if (u.starter && engine_omega_ * kRadToRpm < engine.idle_rpm) engine_torque += engine.starter_torque;
    fuel_ += et.indicated * engine_omega_ * dt * kBsfc;
    body_tm_ = BodyTelemetry{};
    body_tm_.engine_torque = engine_torque;
    body_tm_.fuel_rate = et.indicated * engine_omega_ * kBsfc;

    const bool driven = ratio != 0.0f && has_drive;
    float root_torque = 0.0f, engine_accel = engine_torque / engine.inertia, slip = 0.0f;
    clutch_locked_ = false;
    if (driven) {
        const Affine root = reduce(spec_.driveline, drive, a_, b_);
        const float a_in = root.a * ratio * ratio, b_in = root.b * ratio;  // driveline as seen from the gearbox input
        const float capacity = spec_.clutch.max_torque * bite;
        slip = engine_omega_ - shaft_ * ratio;
        const float accel = (a_in * engine_torque + b_in) / (1.0f + a_in * engine.inertia);
        const float hold = engine_torque - engine.inertia * accel;  // clutch torque needed to stay locked
        float clutch_torque;
        if (std::abs(slip) < kLockSpeed && std::abs(hold) <= capacity) {
            clutch_locked_ = true;
            clutch_torque = hold;
            engine_accel = accel;
        } else {
            clutch_torque = capacity * sign(std::abs(slip) >= kLockSpeed ? slip : hold);
            engine_accel = (engine_torque - clutch_torque) / engine.inertia;
        }
        root_torque = ratio * clutch_torque;
        body_tm_.clutch_torque = clutch_torque;
        body_tm_.clutch_capacity = capacity;
        body_tm_.clutch_slip_rpm = slip * kRadToRpm;
    }
    std::fill(torque_.begin(), torque_.end(), 0.0f);
    distribute(spec_.driveline, drive, root_torque, a_, b_, torque_);

    float traction = 0.0f;
    float hold = 0.0f;  // static grip of wheels held still by their brakes: what a parked vehicle can resist
    for (std::size_t i = 0; i < n; ++i) {
        float next = omega_[i] + (a_[i] * torque_[i] + b_[i]) * dt;
        if (braking(u, spec_.wheels[i]) > 0.0f && next * omega_[i] < 0.0f) next = 0.0f;  // brakes stop a wheel, they don't reverse it
        const float wheel_traction = std::clamp(f_[i] + df_[i] * (next - omega_[i]), -fmax_[i], fmax_[i]);
        traction += wheel_traction;
        wheel_tm_[i].traction = wheel_traction;
        wheel_tm_[i].drive_torque = torque_[i];
        wheel_tm_[i].omega = next;
        omega_[i] = next;
        const float held = braking(u, spec_.wheels[i]);
        if (held > 0.0f && std::abs(next) < kBrakeBand) hold += std::min(fmax_[i], held * spec_.brake_torque / r);
        if (diggable_[i]) {
            const WheelSpec& w = spec_.wheels[i];
            terrain_->dig(x_ + w.x, y_ + w.y, tyre.width, tyre.length, soil_.dig_rate * std::abs(next * r - v_) * dt);
        }
    }

    if (driven) {
        const float input = shaft_speed(spec_.driveline, drive, omega_) * ratio;
        engine_omega_ += engine_accel * dt;
        if (clutch_locked_ || (engine_omega_ - input) * slip < 0.0f) engine_omega_ = input;  // locked, or just caught up
    } else {
        engine_omega_ += engine_accel * dt;
    }
    engine_omega_ = std::max(engine_omega_, 0.0f);
    const float rpm = engine_omega_ * kRadToRpm;
    if (running_ && rpm < engine.stall_rpm) running_ = false;
    else if (!running_ && rpm > 0.7f * engine.idle_rpm) running_ = true;  // starter or bump start

    // Body: one degree of freedom along x.
    const auto [rear, front] = std::minmax_element(spec_.wheels.begin(), spec_.wheels.end(),
                                                   [](const WheelSpec& p, const WheelSpec& q) { return p.x < q.x; });
    const float xf = front->x + 0.5f * tyre.length, xr = rear->x - 0.5f * tyre.length;  // ends of the footprint
    // Slope from the ground under the wheel tracks at either end: not from the centre line, where a heap of gravel
    // between the wheels would read as a hill. The original ground level, raised by whatever lies above it, so a
    // rut is not a gradient, and gravel filling a rut only counts where it stands above the old surface.
    auto ground_at = [&](float wx) {
        float sum = 0.0f;
        for (float ty : track_y_) {
            const Cell& c = terrain_->at(wx, y_ + ty);
            sum += std::max(c.hard + c.soft, c.surface());
        }
        return sum / static_cast<float>(track_y_.size());
    };
    const float rise = ground_at(x_ + xf) - ground_at(x_ + xr);
    const float run = xf - xr;
    const float weight = mass() * kG;
    const float force = traction - weight * rise / std::hypot(run, rise);

    body_tm_.traction = traction;
    body_tm_.resistance = resist;
    body_tm_.grade = -weight * rise / std::hypot(run, rise);
    if (sinkage_ > spec_.clearance) {  // high-centred: the chassis drags on the ground
        body_tm_.belly = kBellyFriction * weight * std::min(1.0f, (sinkage_ - spec_.clearance) / 0.1f);
        resist += body_tm_.belly;
    }
    body_tm_.external = external_;
    resist += external_;

    // Resistance is passive: it opposes motion but never starts it. At a standstill (or reaching one) the tyres
    // of braked wheels that are not turning grip statically too: the slip model alone would let a parked
    // vehicle creep downhill on its tyres' small slip.
    const float stand = resist + hold;
    if (std::abs(v_) < 1e-3f) {
        v_ = std::abs(force) <= stand ? 0.0f : v_ + (force - sign(force) * resist) / mass() * dt;
    } else {
        const float next = v_ + (force - sign(v_) * resist) / mass() * dt;
        v_ = (next * v_ < 0.0f && std::abs(force) <= stand) ? 0.0f : next;
    }
    x_ += v_ * dt;
}

Pose Rig::pose(float x) const {
    const Tyre& tyre = spec_.tyre;
    const float r = spec_.wheel_radius, cs = terrain_->cell_size();
    const std::size_t n = spec_.wheels.size();
    Pose p;
    p.wheel_z.assign(n, 0.0f);
    std::vector<float> slope(n, 0.0f);
    const bool tracked = tyre.length > 2.0f * r;
    for (std::size_t i = 0; i < n; ++i) {
        const WheelSpec& w = spec_.wheels[i];
        const float cx = x + w.x, cy = y_ + w.y;
        if (!tracked) {
            // A wheel's centre sits as high as the highest point under its rim needs: it rolls up a step gradually.
            float centre = -1e30f;
            const float step = 0.5f * cs;
            const int reach = static_cast<int>(r / step);
            for (int k = -reach; k <= reach; ++k) {  // symmetric, and through the centre
                const float d = static_cast<float>(k) * step;
                centre = std::max(centre, terrain_->at(cx + d, cy).surface() + std::sqrt(std::max(0.0f, r * r - d * d)));
            }
            p.wheel_z[i] = centre - r;
            continue;
        }
        // A track is a rigid beam: it lies on the upper convex hull of the ground under it, on the hull segment
        // that spans its centre (where its weight acts). Over a crest it tips from one segment to the next.
        std::vector<std::pair<float, float>> hull;
        const float half = 0.5f * tyre.length;
        for (float d = -half; d <= half + 1e-4f; d += 0.5f * cs) {
            const std::pair<float, float> q{d, terrain_->at(cx + d, cy).surface()};
            while (hull.size() >= 2) {  // monotone chain, upper side: drop points below the line to the new one
                const auto& a = hull[hull.size() - 2];
                const auto& b = hull.back();
                if ((b.first - a.first) * (q.second - a.second) - (b.second - a.second) * (q.first - a.first) >= 0.0f) hull.pop_back();
                else break;
            }
            hull.push_back(q);
        }
        std::size_t k = 0;
        while (k + 2 < hull.size() && hull[k + 1].first < 0.0f) ++k;
        const auto& a = hull[k];
        const auto& b = hull[std::min(k + 1, hull.size() - 1)];
        slope[i] = b.first > a.first ? (b.second - a.second) / (b.first - a.first) : 0.0f;
        p.wheel_z[i] = a.second + slope[i] * (0.0f - a.first);
    }

    // The body: tracks give their own slope; wheels a least-squares line through their contacts. Roll from the
    // mean height of each side.
    float k = 0.0f, z0 = 0.0f;
    if (tracked) {
        for (std::size_t i = 0; i < n; ++i) k += slope[i], z0 += p.wheel_z[i] - slope[i] * spec_.wheels[i].x;
        k /= static_cast<float>(n), z0 /= static_cast<float>(n);
    } else {
        float sx = 0.0f, sz = 0.0f, sxx = 0.0f, sxz = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float wx = spec_.wheels[i].x;
            sx += wx, sz += p.wheel_z[i], sxx += wx * wx, sxz += wx * p.wheel_z[i];
        }
        const float m = static_cast<float>(n), den = m * sxx - sx * sx;
        k = std::abs(den) > 1e-6f ? (m * sxz - sx * sz) / den : 0.0f;
        z0 = (sz - k * sx) / m;
    }
    p.z = z0;
    p.pitch = std::atan(k);
    float left = 0.0f, right = 0.0f, yl = 0.0f, yr = 0.0f;
    int nl = 0, nr = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float wy = spec_.wheels[i].y;
        if (wy < 0.0f) left += p.wheel_z[i], yl += wy, ++nl;
        else right += p.wheel_z[i], yr += wy, ++nr;
    }
    if (nl > 0 && nr > 0) p.roll = std::atan2(right / nr - left / nl, yr / nr - yl / nl);
    return p;
}

void Rig::retune(VehicleSpec spec, SoilParams soil) {
    spec_ = std::move(spec);
    soil_ = soil;
    track_y_.clear();
    for (const WheelSpec& w : spec_.wheels)
        if (std::find(track_y_.begin(), track_y_.end(), w.y) == track_y_.end()) track_y_.push_back(w.y);
    const int top = static_cast<int>(spec_.gearbox.forward.size()), bottom = -static_cast<int>(spec_.gearbox.reverse.size());
    gear_ = std::clamp(gear_, bottom, top);  // fewer gears than before: stay in the highest there is
}

float Rig::engine_rpm() const { return engine_omega_ * kRadToRpm; }

float Rig::input_rpm(int gear) const { return shaft_ * spec_.gearbox.ratio(gear, low_) * kRadToRpm; }

RunResult drive_to(Rig& rig, const Driver& d, float goal_x, float timeout, float dt) {
    ShiftController box;
    DriverInput in;
    in.gear = 1;
    in.low_range = d.low_range;
    in.awd = d.awd;
    in.diff_lock = d.diff_lock;

    const float fuel0 = rig.fuel_kg();
    float t = 0.0f, check_t = 0.0f, check_x = rig.x();
    while (rig.x() < goal_x && t < timeout) {
        in.throttle = std::clamp((d.target_speed - rig.speed()) * 2.0f, 0.0f, 1.0f);
        rig.step(box.update(in, rig, dt), dt);
        t += dt;
        if (t - check_t >= 10.0f) {  // stuck: less than half a metre in ten seconds
            if (rig.x() - check_x < 0.5f) break;
            check_t = t;
            check_x = rig.x();
        }
    }
    return {rig.x() >= goal_x, t, rig.x(), rig.fuel_kg() - fuel0};
}

}  // namespace inc
