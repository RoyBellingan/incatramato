// Playable side-view prototype of the core loop. Flat colours, keyboard only, one lane.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "font5x7.h"
#include "input.h"
#include "inc/world.h"
#include "sound.h"
#include "telemetry.h"

using namespace inc;

namespace {

constexpr int kW = 1280, kH = 720;
constexpr float kScale = 40.0f;  // pixels per metre in the side view
constexpr float kViewTop = 140.0f, kGroundY = 470.0f, kViewBottom = 590.0f, kCamScreenX = 440.0f;
constexpr const char* kBindingsFile = "input.cfg";  // controller bindings, in the working directory
constexpr Uint32 kDoubleTapMs = 350;  // two brake presses this close together toggle the handbrake

struct Rgb {
    Uint8 r, g, b;
};
constexpr Rgb kSky{176, 205, 230}, kFirm{139, 115, 85}, kMud{70, 48, 30}, kGravel{150, 150, 140}, kLaid{92, 92, 98},
    kRolled{38, 38, 42}, kPanel{24, 26, 30}, kText{225, 225, 220}, kDim{120, 124, 130}, kGreen{70, 190, 90},
    kAmber{235, 170, 40}, kRed{225, 60, 50}, kTyre{28, 28, 30}, kSteel{150, 152, 158}, kOff{52, 54, 60};

Rgb shade(Rgb c, float k) {
    return {static_cast<Uint8>(c.r * k), static_cast<Uint8>(c.g * k), static_cast<Uint8>(c.b * k)};
}

// ---------------------------------------------------------------- the local client

// What only this screen keeps about a vehicle: its paint, where it was at the start of the last tick (to draw
// it smoothly between ticks), and how far its wheels have turned.
struct Look {
    Rgb paint;
    float x_prev;
    std::vector<float> wheel_angle;
};

// One player in a world, plus what only this screen needs. The world holds every rule; this turns keys into
// commands and controls, events into messages, and ticks the world as real time passes.
struct Game {
    std::unique_ptr<World> world = make_playground();
    PlayerId me = 0;
    std::vector<Look> looks;
    std::string message = "DRIVE TO THE TOWN. TAB SWITCHES VEHICLE.";
    float message_time = 6.0f;
    float key_throttle = 0.0f, key_brake = 0.0f, key_clutch = 0.0f;  // keyboard pedals, ramped
    float tick_debt = 0.0f;  // real time not yet simulated, less than one tick after update()
    float alpha = 0.0f;      // how far the picture is between the last two ticks, 0..1
    std::string last_trip;

    Game() {
        me = world->join();
        const Rgb paints[] = {{222, 172, 44}, {200, 84, 52}, {236, 196, 36}};
        for (std::size_t i = 0; i < world->machines().size(); ++i) {
            const Machine& m = world->machines()[i];
            looks.push_back({paints[i % 3], m.rig.x(), std::vector<float>(m.rig.spec().wheels.size(), 0.0f)});
        }
        world->take_events();  // being seated at the start needs no message
    }
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    int active() const { return world->vehicle_of(me); }
    Machine& car() { return world->machine(active()); }
    const Machine& car() const { return world->machine(active()); }
    float shown_x(int i) const {  // where to draw vehicle i: between its last two ticks
        const float x = world->machine(i).rig.x(), x0 = looks[static_cast<std::size_t>(i)].x_prev;
        return std::abs(x - x0) > 2.0f ? x : x0 + (x - x0) * alpha;  // a jump (recovery) is not smoothed
    }
    void say(std::string text) {
        message = std::move(text);
        message_time = 3.0f;
    }
};

struct Held {
    bool throttle = false, brake = false, clutch = false, starter = false;  // keys or buttons
    bool cargo = false;  // G held: tip gravel
    float throttle_axis = 0.0f, brake_axis = 0.0f, clutch_axis = 0.0f;     // analogue pedals and levers
};

const char* mode_name(ShiftMode m) {
    switch (m) {
        case ShiftMode::Manual: return "MANUAL";
        case ShiftMode::Sequential: return "SEQUENTIAL";
        case ShiftMode::Automatic: break;
    }
    return "AUTOMATIC";
}

// The HUD's words for what the world reports.
std::string event_text(const Event& e, const World& w) {
    using K = Event::Kind;
    char buf[96];
    switch (e.kind) {
        case K::GearboxMode: return std::string("GEARBOX: ") + mode_name(static_cast<ShiftMode>(static_cast<int>(e.value)));
        case K::Awd: return e.on ? "AWD ENGAGED" : "AWD OFF";
        case K::DiffLock: return e.on ? "DIFFS LOCKED" : "DIFFS OPEN";
        case K::LowRange: return e.on ? "LOW RANGE SELECTED (STOP OR CLUTCH TO ENGAGE)" : "HIGH RANGE SELECTED";
        case K::EngineBrake: return e.on ? "ENGINE BRAKE ON" : "ENGINE BRAKE OFF";
        case K::Handbrake: return e.on ? "HANDBRAKE ON (DOUBLE-TAP S TO RELEASE)" : "HANDBRAKE OFF";
        case K::Seated: return w.machine(e.vehicle).label;
        case K::NoFreeVehicle: return "EVERY OTHER VEHICLE HAS A DRIVER";
        case K::Blade:
            if (!e.on) return e.value2 > 0.5f ? "BLADE UP FOR REVERSING (G TO LOWER)" : "BLADE UP";
            std::snprintf(buf, sizeof buf, "BLADE DOWN: SPREADING %.0f CM (Z/X THINNER/THICKER)", e.value * 100.0f);
            return buf;
        case K::BladeLayer:
            if (e.value < -0.01f) std::snprintf(buf, sizeof buf, "BLADE %.0f CM BELOW GROUND: SCRAPES RUTS, RIPS UP ROAD", -e.value * 100.0f);
            else if (e.value < 0.01f) std::snprintf(buf, sizeof buf, "BLADE AT GROUND LEVEL: STRIPPING GRAVEL");
            else std::snprintf(buf, sizeof buf, "BLADE LAYER %.0f CM", e.value * 100.0f);
            return buf;
        case K::Loaded:
            std::snprintf(buf, sizeof buf, "LOADED %.0f M3 OF GRAVEL", e.value);
            return buf;
        case K::HoldToTip: return "HOLD G TO TIP, LET GO TO STOP";
        case K::BedEmpty: return "EMPTY. LOAD AT THE QUARRY.";
        case K::TipBlocked: return "TOO CLOSE TO ANOTHER VEHICLE: NO TIPPING HERE";
        case K::Paved:
            if (e.value > 0.0f) std::snprintf(buf, sizeof buf, "LAID %.0f M2 OF ASPHALT. ROLL IT (O).", e.value);
            else std::snprintf(buf, sizeof buf, "NOTHING TO PAVE: NEEDS %.0f CM OF GRADED GRAVEL", e.value2 * 100.0f);
            return buf;
        case K::Rolled:
            if (e.value > 0.0f) std::snprintf(buf, sizeof buf, "ROLLED %.0f M2", e.value);
            else return "NOTHING TO ROLL";
            return buf;
        case K::Recovered: return "RECOVERED TO THE QUARRY";
        case K::Arrived: return "ARRIVED IN TOWN";
    }
    return "";
}

// Shows the world's events that concern this player.
void show_events(Game& g) {
    for (const Event& e : g.world->take_events()) {
        if (e.player != g.me) continue;
        if (e.kind == Event::Kind::Arrived) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "LAST TRIP %.1f S  %.2f L", e.value, e.value2);
            g.last_trip = buf;
        }
        g.say(event_text(e, *g.world));
    }
}

void send(Game& g, Command c) {
    if (c == Command::NextVehicle) g.key_throttle = g.key_brake = g.key_clutch = 0.0f;  // the keys stay with the old one
    g.world->command(g.me, c);
    show_events(g);
}

void update(Game& g, const Held& h, float frame_dt) {
    auto ramp = [&](float& pedal, bool on, float up, float down) {
        pedal = std::clamp(pedal + (on ? up : -down) * frame_dt, 0.0f, 1.0f);
    };
    ramp(g.key_throttle, h.throttle, 3.0f, 6.0f);
    ramp(g.key_brake, h.brake, 6.0f, 8.0f);
    ramp(g.key_clutch, h.clutch, 10.0f, 1.6f);  // stamps down fast, comes up slowly so a key can feather it
    PlayerControls c;
    c.throttle = std::max(g.key_throttle, h.throttle_axis);
    c.brake = std::max(g.key_brake, h.brake_axis);
    c.clutch = std::max(g.key_clutch, h.clutch_axis);
    c.starter = h.starter;
    c.tip = h.cargo;
    g.world->controls(g.me, c);

    // The world advances in fixed ticks; the leftover time carries to the next frame, and the picture is drawn
    // that far between the last two ticks.
    g.tick_debt += frame_dt;
    while (g.tick_debt >= kTickDt) {
        for (std::size_t i = 0; i < g.looks.size(); ++i) g.looks[i].x_prev = g.world->machines()[i].rig.x();
        g.world->tick();
        g.tick_debt -= kTickDt;
    }
    g.alpha = g.tick_debt / kTickDt;
    show_events(g);

    for (std::size_t i = 0; i < g.looks.size(); ++i) {
        const auto speeds = g.world->machines()[i].rig.wheel_speeds();
        for (std::size_t k = 0; k < g.looks[i].wheel_angle.size(); ++k)
            g.looks[i].wheel_angle[k] = std::fmod(g.looks[i].wheel_angle[k] + speeds[k] * frame_dt, 6.2831853f);
    }
    if (g.car().ripping) g.say("RIPPING UP THE ROAD");
    if (g.car().tipping) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "TIPPING: %.1f M3 LEFT", g.car().gravel);
        g.say(buf);
    }
    g.message_time = std::max(0.0f, g.message_time - frame_dt);
}

// Controller state: devices, bindings, the setup wizard, and what was pressed last frame.
struct Pad {
    input::Devices devices;
    input::Map map;
    input::Wizard wizard;
    std::array<bool, input::kFnCount> was{};
    int gate = input::Map::kNoShifter;
};

// Feeds bound axes and buttons into the game. Pedals add to the keyboard; buttons act like their keys;
// an H-shifter moves the lever when it changes gate, including back to neutral.
void poll_pad(Pad& pad, Game& g, Held& h) {
    static constexpr std::pair<int, Command> kButtons[] = {
        {input::ShiftUp, Command::ShiftUp}, {input::ShiftDown, Command::ShiftDown}, {input::Mode, Command::NextMode}, {input::Awd, Command::ToggleAwd},
        {input::Lock, Command::ToggleLock}, {input::Low, Command::ToggleLow}, {input::EngineBrake, Command::ToggleEngineBrake},
        {input::NextVehicle, Command::NextVehicle}, {input::Cargo, Command::Cargo}, {input::Pave, Command::Pave}, {input::Roll, Command::Roll},
        {input::Recover, Command::Recover}};
    const input::Map& m = pad.map;
    h.throttle_axis = m.axis(input::Throttle, pad.devices);
    h.brake_axis = m.axis(input::Brake, pad.devices);
    h.clutch_axis = m.axis(input::Clutch, pad.devices);
    h.throttle |= m.button(input::Throttle, pad.devices);
    h.brake |= m.button(input::Brake, pad.devices);
    h.clutch |= m.button(input::Clutch, pad.devices);
    h.starter |= m.button(input::Starter, pad.devices);
    h.cargo |= m.button(input::Cargo, pad.devices);
    for (const auto& [fn, action] : kButtons) {
        const bool down = m.button(fn, pad.devices);
        if (down && !pad.was[fn]) send(g, action);
        pad.was[fn] = down;
    }
    const int gate = m.gate(pad.devices);
    if (gate != pad.gate && gate != input::Map::kNoShifter)
        send(g, gate < 0 ? Command::Reverse : static_cast<Command>(static_cast<int>(Command::Gear0) + gate));
    pad.gate = gate;
}

SoundState sound_state(const Game& g, const Held& h) {
    const Machine& v = g.car();
    const Rig& rig = v.rig;
    SoundState s;
    s.rpm = rig.engine_rpm();
    s.throttle = v.out.throttle;
    for (float w : rig.wheel_speeds()) s.slip = std::max(s.slip, std::abs(w * rig.spec().wheel_radius - rig.speed()));
    s.running = rig.engine_running();
    s.grinding = rig.grinding();
    s.starter = h.starter;
    return s;
}

// ---------------------------------------------------------------- drawing

struct Painter {
    SDL_Renderer* r;

    void colour(Rgb c) const { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); }
    void rect(float x, float y, float w, float h, Rgb c) const {
        colour(c);
        const SDL_FRect q{x, y, w, h};
        SDL_RenderFillRectF(r, &q);
    }
    void frame(float x, float y, float w, float h, Rgb c) const {
        colour(c);
        const SDL_FRect q{x, y, w, h};
        SDL_RenderDrawRectF(r, &q);
    }
    void line(float x0, float y0, float x1, float y1, Rgb c) const {
        colour(c);
        SDL_RenderDrawLineF(r, x0, y0, x1, y1);
    }
    void poly(const SDL_FPoint* p, int n, Rgb c) const {  // convex
        std::vector<SDL_Vertex> v;
        const SDL_Color col{c.r, c.g, c.b, 255};
        for (int i = 1; i + 1 < n; ++i)
            for (int k : {0, i, i + 1}) v.push_back({p[k], col, {0.0f, 0.0f}});
        SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), nullptr, 0);
    }
    void disc(float cx, float cy, float radius, Rgb c) const {
        SDL_FPoint p[24];
        for (int i = 0; i < 24; ++i) {
            const float a = i * 6.2831853f / 24.0f;
            p[i] = {cx + radius * std::cos(a), cy + radius * std::sin(a)};
        }
        poly(p, 24, c);
    }
    float text(float x, float y, int s, Rgb c, const char* str) const {
        colour(c);
        for (; *str; ++str, x += 6.0f * s) {
            const unsigned char* g = font5x7::glyph(*str);
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g[row] & (0x10 >> col)) {
                        const SDL_FRect q{x + col * s, y + row * s, static_cast<float>(s), static_cast<float>(s)};
                        SDL_RenderFillRectF(r, &q);
                    }
        }
        return x;
    }
    void centred(float cx, float y, int s, Rgb c, const char* str) const {
        text(cx - 3.0f * s * static_cast<float>(std::strlen(str)), y, s, c, str);
    }
};

struct View {
    float cam_x;
    float sx(float wx) const { return kCamScreenX + (wx - cam_x) * kScale; }
    float sy(float wz) const { return kGroundY - wz * kScale; }
};

void draw_ground(const Painter& p, const Game& g, View view) {
    const Terrain& t = g.world->terrain();
    const float cs = t.cell_size();
    const float lane_y = g.world->course().lane_y, track_y = lane_y - 1.0f;  // the left wheel track
    p.rect(0, kViewTop, kW, kViewBottom - kViewTop, kSky);

    // Scenery: the quarry heap, the town, and a post every ten metres.
    const SDL_FPoint heap[3] = {{view.sx(28), view.sy(0)}, {view.sx(35), view.sy(3.2f)}, {view.sx(42), view.sy(0)}};
    p.poly(heap, 3, shade(kGravel, 0.9f));
    p.centred(view.sx(35), view.sy(4.4f), 2, kDim, "QUARRY");
    p.rect(view.sx(128), view.sy(3.0f), 6.0f * kScale, 3.0f * kScale, {196, 180, 160});
    const SDL_FPoint roof[3] = {{view.sx(127.5f), view.sy(3.0f)}, {view.sx(131), view.sy(4.6f)}, {view.sx(134.5f), view.sy(3.0f)}};
    p.poly(roof, 3, {150, 70, 60});
    p.centred(view.sx(131), view.sy(5.6f), 2, kDim, "TOWN");
    for (float m = std::ceil((view.cam_x - 12.0f) / 10.0f) * 10.0f; m < view.cam_x + 22.0f; m += 10.0f) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f", m);
        p.rect(view.sx(m) - 1.0f, view.sy(1.0f), 2.0f, kScale, kDim);
        p.centred(view.sx(m), view.sy(1.0f) - 12.0f, 1, kDim, buf);
    }

    for (float wx = std::floor((view.cam_x - 11.5f) / cs) * cs; wx < view.cam_x + 21.5f; wx += cs) {
        const Cell& c = t.at(wx + 0.5f * cs, track_y);
        const float x0 = view.sx(wx), w = cs * kScale + 1.0f;
        const float y_hard = view.sy(c.hard), y_mud = view.sy(c.mud_top()), y_base = view.sy(c.mud_top() + c.gravel);
        const float y_top = view.sy(c.surface());
        p.rect(x0, y_hard, w, kViewBottom - y_hard, kFirm);
        if (c.soft > 0.0f) p.rect(x0, y_mud, w, y_hard - y_mud + 1.0f, kMud);  // +1: no seams between layers
        if (c.gravel > 0.005f) p.rect(x0, y_base, w, y_mud - y_base + 1.0f, kGravel);
        if (c.paving != Paving::None) p.rect(x0, y_base - 4.0f, w, 5.0f, c.paving == Paving::Rolled ? kRolled : kLaid);
        if (c.loose > 0.005f) p.rect(x0, y_top, w, y_base - 4.0f - y_top + 1.0f, kGravel);  // loose gravel on the road
    }
    // The ground along the middle of the lane, between the wheels, as a line: the side view shows the left wheel
    // track, so a heap or crown under the chassis would otherwise be invisible.
    for (float wx = std::floor((view.cam_x - 11.5f) / cs) * cs; wx < view.cam_x + 21.5f; wx += cs) {
        const Cell& c = t.at(wx + 0.5f * cs, lane_y);
        p.rect(view.sx(wx), view.sy(c.surface()) - 1.0f, cs * kScale * 0.6f, 2.0f, {235, 235, 225});
    }
}

void draw_wheel(const Painter& p, float cx, float cy, float radius, float angle) {
    p.disc(cx, cy, radius, kTyre);
    p.disc(cx, cy, radius * 0.5f, kSteel);
    p.line(cx, cy, cx + std::cos(angle) * radius * 0.92f, cy + std::sin(angle) * radius * 0.92f, kText);
}

void draw_vehicle(const Painter& p, const Game& g, int index, View view, bool active) {
    const Machine& v = g.world->machine(index);
    const Look& look = g.looks[static_cast<std::size_t>(index)];
    const Terrain& t = g.world->terrain();
    const VehicleSpec& s = v.rig.spec();
    const float r = s.wheel_radius, x = g.shown_x(index);
    const float track_y = v.rig.y() - 1.0f;
    auto ground = [&](float wx) { return t.at(wx, track_y).surface(); };
    if (view.sx(x + 8.0f) < 0.0f || view.sx(x - 8.0f) > kW) return;

    const Pose pose = v.rig.pose(x);
    if (v.has_blade) {
        // Everything on the dozer is laid out along its tilted track line: a metres along it, b above it.
        const float half = 0.5f * s.tyre.length, c = std::cos(pose.pitch), sn = std::sin(pose.pitch);
        auto at = [&](float a, float b) { return SDL_FPoint{view.sx(x + a * c - b * sn), view.sy(pose.z + a * sn + b * c)}; };
        auto box = [&](float a0, float a1, float b0, float b1, Rgb col) {
            const SDL_FPoint q[4] = {at(a0, b0), at(a1, b0), at(a1, b1), at(a0, b1)};
            p.poly(q, 4, col);
        };
        box(-half, half, 0.0f, 2.0f * r, kTyre);
        box(-half - 0.2f, half + 0.2f, 2.0f * r, 2.0f * r + 1.1f, look.paint);
        box(-1.0f, 0.6f, 2.0f * r + 1.1f, 2.0f * r + 2.1f, shade(look.paint, 0.8f));
        box(-0.8f, 0.4f, 2.0f * r + 1.25f, 2.0f * r + 1.85f, {200, 225, 240});
        const SDL_FPoint rear = at(-half, r), front = at(half, r);
        draw_wheel(p, rear.x, rear.y, r * kScale, look.wheel_angle[0]);
        draw_wheel(p, front.x, front.y, r * kScale, look.wheel_angle[0]);
        // The blade hangs at its cutting edge when down (where the ground model cuts), lifted clear when up.
        const float bx = x + kBladeAhead, bz = v.blade ? v.blade_edge : pose.z + std::tan(pose.pitch) * kBladeAhead + 0.5f;
        p.rect(view.sx(bx - 0.09f), view.sy(bz + 1.1f), 0.18f * kScale, 1.1f * kScale, kSteel);
        const SDL_FPoint arm = at(half, 0.7f);
        p.line(arm.x, arm.y, view.sx(bx), view.sy(bz + 0.5f), kSteel);
        const SDL_FPoint mark = at(0.0f, 2.0f * r + 2.9f);
        if (active) p.centred(mark.x, mark.y, 2, kText, "V");
        return;
    }

    // Truck: the body rides on a line from the rear bogie to the front axle, so it pitches as wheels sink.
    float front_x = -1e9f;
    for (const WheelSpec& w : s.wheels) front_x = std::max(front_x, w.x);
    float fz = 0.0f, rx = 0.0f, rz = 0.0f;
    int rear_count = 0;
    for (const WheelSpec& w : s.wheels) {
        if (w.y > 0.0f) continue;
        const float wz = pose.wheel_z[static_cast<std::size_t>(&w - s.wheels.data())] + r;  // rolls up steps, not jumps
        if (w.x == front_x) {
            fz = wz;
        } else {
            rx += w.x;
            rz += wz;
            ++rear_count;
        }
    }
    rx /= static_cast<float>(rear_count);
    rz /= static_cast<float>(rear_count);
    const float len = std::hypot(front_x - rx, fz - rz);
    const float dx = (front_x - rx) / len, dz = (fz - rz) / len;
    auto at = [&](float a, float b) {  // a metres along the body from the bogie, b metres above the axle line
        return SDL_FPoint{view.sx(x + rx + dx * a - dz * b), view.sy(rz + dz * a + dx * b)};
    };
    auto box = [&](float a0, float a1, float b0, float b1, Rgb c) {
        const SDL_FPoint q[4] = {at(a0, b0), at(a1, b0), at(a1, b1), at(a0, b1)};
        p.poly(q, 4, c);
    };
    box(-2.2f, len + 1.4f, 0.15f, 0.45f, {60, 62, 66});
    box(-2.2f, len - 1.0f, 0.45f, 1.5f, shade(look.paint, 0.7f));
    if (v.gravel > 0.0f) box(-2.1f, len - 1.1f, 0.55f, 0.55f + 0.85f * v.gravel / v.capacity, kGravel);
    box(len - 0.8f, len + 1.4f, 0.45f, 2.3f, look.paint);
    box(len + 0.3f, len + 1.25f, 1.45f, 2.1f, {200, 225, 240});
    for (std::size_t i = 0; i < s.wheels.size(); ++i) {
        const WheelSpec& w = s.wheels[i];
        if (w.y > 0.0f) continue;
        draw_wheel(p, view.sx(x + w.x), view.sy(pose.wheel_z[i] + r), r * kScale * (s.tyre.width > 0.4f ? 1.08f : 1.0f),
                   look.wheel_angle[i]);
    }
    if (active) {
        const SDL_FPoint mark = at(len + 0.3f, 3.1f);
        p.centred(mark.x, mark.y, 2, kText, "V");
    }

    // Where the load lands: a marker over the spot while there is gravel in the bed, and the stream while tipping.
    if (active && v.gravel > 0.0f) {
        const float lx = x + kTipAhead, lz = ground(lx);
        const Rgb c = x < g.world->quarry_end() ? kDim : kAmber;  // no tipping in the quarry
        if (v.tipping) p.rect(view.sx(lx - 0.25f), view.sy(lz + 2.4f), 0.5f * kScale, 2.4f * kScale, kGravel);
        const SDL_FPoint arrow[3] = {{view.sx(lx - 0.35f), view.sy(lz + 3.2f)}, {view.sx(lx + 0.35f), view.sy(lz + 3.2f)},
                                     {view.sx(lx), view.sy(lz + 2.6f)}};
        p.poly(arrow, 3, c);
        p.centred(view.sx(lx), view.sy(lz + 3.2f) - 14.0f, 1, c, "TIP");
    }
}

void lamp(const Painter& p, float x, float y, const char* label, bool on, Rgb lit) {
    p.rect(x, y, 78, 24, on ? lit : kOff);
    p.centred(x + 39, y + 5, 2, on ? kPanel : kDim, label);
}

void draw_hud(const Painter& p, const Game& g) {
    const Machine& v = g.car();
    const Rig& rig = v.rig;
    const Engine& e = rig.spec().engine;
    char buf[96];
    p.rect(0, 0, kW, kViewTop, kPanel);

    // Tachometer.
    const float rpm = rig.engine_rpm(), span = e.redline_rpm * 1.1f;
    const float bx = 20, by = 44, bw = 380, bh = 30;
    p.rect(bx, by, bw, bh, kOff);
    p.rect(bx + bw * e.redline_rpm / span, by, bw - bw * e.redline_rpm / span, bh, shade(kRed, 0.45f));
    p.rect(bx, by, bw * std::min(rpm / span, 1.0f), bh, rpm > e.redline_rpm ? kRed : rpm > 0.8f * e.redline_rpm ? kAmber : kGreen);
    p.rect(bx + bw * e.idle_rpm / span, by - 4, 2, bh + 8, kText);
    for (float m = 500.0f; m < span; m += 500.0f) p.rect(bx + bw * m / span, by + bh, 1, 6, kDim);
    std::snprintf(buf, sizeof buf, "%4.0f RPM", rpm);
    p.text(bx, 16, 3, kText, buf);
    p.text(bx, 92, 2, kDim, v.label.c_str());
    if (!rig.engine_running()) p.text(bx, 114, 2, kRed, "ENGINE STALLED - HOLD I TO START");
    else if (rig.telemetry().belly > 0.0f) p.text(bx, 114, 2, kRed, "HIGH-CENTRED: THE CHASSIS IS DRAGGING");

    // Speed and gear.
    std::snprintf(buf, sizeof buf, "%3.0f", std::abs(rig.speed()) * 3.6f);
    p.text(425, 20, 6, kText, buf);
    p.text(540, 48, 2, kDim, "KM/H");
    const int gear = rig.gear();
    buf[0] = gear < 0 ? 'R' : gear == 0 ? 'N' : static_cast<char>('0' + gear);
    buf[1] = '\0';
    p.rect(612, 12, 76, 84, kOff);
    p.text(629, 22, 9, rig.grinding() ? kRed : kText, buf);
    if (gear < 0 && rig.spec().gearbox.reverse.size() > 1) {  // which reverse, on a box with several
        const char r[2] = {static_cast<char>('0' - gear), '\0'};
        p.text(676, 76, 2, kText, r);
    }
    if (v.box.selected() != gear) {
        const int want = v.box.selected();
        std::snprintf(buf, sizeof buf, "> %c", want < 0 ? 'R' : want == 0 ? 'N' : static_cast<char>('0' + want));
        p.text(696, 20, 3, kAmber, buf);
    }
    p.centred(650, 104, 2, kText, mode_name(v.box.mode));
    if (rig.low_range()) p.text(696, 60, 2, kAmber, "LOW");
    if (v.in.handbrake) p.text(696, 80, 2, kRed, "HBRK");

    // Pedals: what the actuators are doing, so the automated clutch is visible.
    const char* names[3] = {"C", "B", "T"};
    const float pedals[3] = {v.out.clutch, v.out.brake, v.out.throttle};
    const Rgb colours[3] = {kAmber, kRed, kGreen};
    for (int i = 0; i < 3; ++i) {
        const float px = 770.0f + i * 34.0f;
        p.rect(px, 14, 24, 84, kOff);
        p.rect(px, 14 + 84 * (1.0f - pedals[i]), 24, 84 * pedals[i], colours[i]);
        p.text(px + 6, 104, 2, kDim, names[i]);
    }

    // Lamps.
    const bool slipping = gear != 0 && !rig.clutch_locked() && v.out.clutch < 0.79f;
    lamp(p, 890, 14, "AWD", v.in.awd, kGreen);
    lamp(p, 974, 14, "LOCK", v.in.diff_lock, kAmber);
    lamp(p, 890, 44, "LOW", rig.low_range(), kAmber);
    lamp(p, 974, 44, "E-BRK", v.in.engine_brake, kGreen);
    lamp(p, 890, 74, "SLIP", slipping, kAmber);
    lamp(p, 974, 74, rig.engine_running() ? "GRIND" : "STALL", rig.grinding() || !rig.engine_running(), kRed);

    // Wheels: green grips, amber slips, red spins. Front of the vehicle is on the right.
    const VehicleSpec& s = rig.spec();
    p.text(890, 108, 2, kDim, "WHEELS");
    for (std::size_t i = 0; i < s.wheels.size(); ++i) {
        const float slip = std::abs(rig.wheel_speeds()[i] * s.wheel_radius - rig.speed()) / std::max(std::abs(rig.speed()), 1.0f);
        const float wx = 1010.0f + (s.wheels[i].x - s.wheels.back().x) * 9.0f, wy = s.wheels[i].y < 0.0f ? 104.0f : 120.0f;
        p.rect(wx, wy, 10, 12, slip < 0.15f ? kGreen : slip < 0.5f ? kAmber : kRed);
    }

    // Numbers.
    std::snprintf(buf, sizeof buf, "SINK %2.0f CM / CLEARANCE %2.0f", rig.mean_sinkage() * 100.0f, s.clearance * 100.0f);
    p.text(1076, 16, 1, rig.mean_sinkage() > s.clearance ? kRed : kText, buf);
    std::snprintf(buf, sizeof buf, "FUEL USED %.2f L", rig.fuel_kg() / 0.84f);
    p.text(1076, 32, 1, kText, buf);
    if (v.has_blade && v.blade)
        std::snprintf(buf, sizeof buf, "BLADE %.0f CM  PUSHING %.1f M3", v.blade_layer * 100.0f, v.blade_pile);
    else if (v.has_blade) std::snprintf(buf, sizeof buf, "BLADE UP (%.0f CM SET)", v.blade_layer * 100.0f);
    else std::snprintf(buf, sizeof buf, "GRAVEL %.0f / %.0f M3", v.gravel, v.capacity);
    p.text(1076, 48, 1, kText, buf);
    std::snprintf(buf, sizeof buf, "X %.0f M", rig.x());
    p.text(1076, 64, 1, kText, buf);
    if (v.trip_running) {
        std::snprintf(buf, sizeof buf, "TRIP %.1f S", v.trip_time);
        p.text(1076, 80, 1, kAmber, buf);
    }
    p.text(1076, 96, 1, kDim, g.last_trip.c_str());

    if (g.message_time > 0.0f) p.centred(kW / 2.0f, kViewTop + 14, 3, kPanel, g.message.c_str());
}

Uint32 map_colour(const Cell& c) {
    Rgb k = kFirm;
    if (c.paving != Paving::None) {
        k = c.paving == Paving::Rolled ? kRolled : kLaid;
        const float cover = std::clamp(c.loose / 0.15f, 0.0f, 1.0f);
        k = {static_cast<Uint8>(std::lerp(k.r, kGravel.r, cover)), static_cast<Uint8>(std::lerp(k.g, kGravel.g, cover)),
             static_cast<Uint8>(std::lerp(k.b, kGravel.b, cover))};
    } else {
        if (c.soft > 0.0f) k = shade(kMud, 1.3f - 0.9f * c.rut / std::max(c.soft, 0.01f));
        const float cover = std::clamp(c.gravel / 0.15f, 0.0f, 1.0f);
        k = {static_cast<Uint8>(std::lerp(k.r, kGravel.r, cover)), static_cast<Uint8>(std::lerp(k.g, kGravel.g, cover)),
             static_cast<Uint8>(std::lerp(k.b, kGravel.b, cover))};
    }
    return 0xFF000000u | (k.r << 16) | (k.g << 8) | k.b;
}

// Top-down map. It is repainted from the same change list a network client would receive.
struct Minimap {
    SDL_Texture* texture = nullptr;
    std::vector<Uint32> pixels;

    void rebuild(SDL_Renderer* r, Terrain& t) {
        if (!texture) texture = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, t.nx(), t.ny());
        pixels.resize(static_cast<std::size_t>(t.nx()) * t.ny());
        for (int iy = 0; iy < t.ny(); ++iy)
            for (int ix = 0; ix < t.nx(); ++ix) pixels[static_cast<std::size_t>(iy) * t.nx() + ix] = map_colour(t.cell(ix, iy));
        t.take_deltas();
        SDL_UpdateTexture(texture, nullptr, pixels.data(), t.nx() * 4);
        ++version;
    }
    std::uint32_t version = 0;  // counts refreshes that found changes: the 3D view reloads the ground when it moves on
    void refresh(Terrain& t) {
        const std::vector<CellDelta> deltas = t.take_deltas();
        if (deltas.empty()) return;
        for (const CellDelta& d : deltas) pixels[d.index] = map_colour(d.cell);
        SDL_UpdateTexture(texture, nullptr, pixels.data(), t.nx() * 4);
        ++version;
    }
};

void draw_map(const Painter& p, const Game& g, const Minimap& map, View view) {
    const Terrain& t = g.world->terrain();
    const float mx = 40, my = 598, mw = 1200, mh = 80;
    const float per_m = mw / (t.nx() * t.cell_size());
    p.rect(0, kViewBottom, kW, kH - kViewBottom, kPanel);
    const SDL_FRect dst{mx, my, mw, mh};
    SDL_RenderCopyF(p.r, map.texture, nullptr, &dst);
    for (int i = 0; i < static_cast<int>(g.looks.size()); ++i) {
        const Machine& v = g.world->machine(i);
        const float x = g.shown_x(i), vx = mx + x * per_m, vy = my + v.rig.y() * per_m;
        p.rect(vx - 20, vy - 8, 36, 16, g.looks[static_cast<std::size_t>(i)].paint);
        if (i == g.active()) p.frame(vx - 22, vy - 10, 40, 20, kText);
        if (i == g.active() && v.gravel > 0.0f)  // where a tipped load will land
            p.rect(mx + (x + kTipAhead) * per_m - 3.0f, my + (g.world->course().lane_y - 0.5f * kTipWidth) * per_m, 6, kTipWidth * per_m, kAmber);
    }
    p.frame(mx + (view.cam_x - 11.0f) * per_m, my - 2, 32.0f * per_m, mh + 4, kText);
    p.centred(mx + 30.0f * per_m, my + mh + 4, 1, kDim, "QUARRY");
    p.centred(mx + 80.0f * per_m, my + mh + 4, 1, kDim, "MUD");
    p.centred(mx + 131.0f * per_m, my + mh + 4, 1, kDim, "TOWN");
    p.text(40, 694, 1, kDim,
           "DOTTED LINE: GROUND BETWEEN THE WHEELS   W/S THROTTLE BRAKE (SS HANDBRAKE)   SHIFT CLUTCH   1-6 N R GEARS   Q/E SHIFT DOWN/UP   M GEARBOX MODE   I STARTER   B ENGINE BRAKE   F3 CONTROLLER SETUP");
    p.text(40, 706, 1, kDim,
           "T AWD   Y DIFF LOCK   L LOW RANGE   TAB VEHICLE   G LOAD / HOLD TO TIP (DOZER: BLADE, Z/X LAYER)   P PAVE   O ROLL   BACKSPACE RECOVER   F5 RESET   F1 SOUND   F11 FULLSCREEN");
}

void draw_wizard(const Painter& p, const Pad& pad) {
    char buf[96];
    const float cx = kW / 2.0f, y = 210.0f;
    p.rect(cx - 400, y, 800, 190, kPanel);
    p.frame(cx - 400, y, 800, 190, kText);
    std::snprintf(buf, sizeof buf, "CONTROLLER SETUP  %d / %d", pad.wizard.current() + 1, static_cast<int>(input::kFnCount));
    p.centred(cx, y + 16, 2, kDim, buf);
    p.centred(cx, y + 50, 5, kAmber, input::kFnNames[pad.wizard.current()]);
    p.centred(cx, y + 104, 2, kText,
              input::is_pedal(pad.wizard.current()) ? "PUSH IT ALL THE WAY, THEN LET GO. A BUTTON WORKS TOO."
                                                    : "PRESS THE BUTTON, OR PUT THE LEVER IN THAT GATE.");
    p.centred(cx, y + 132, 2, kDim, "ESC: SKIP THIS ONE     F3: CANCEL");
    std::string names = "DEVICES:";
    for (SDL_Joystick* j : pad.devices.sticks()) names += " [" + input::Devices::name(j) + "]";
    p.centred(cx, y + 164, 1, kDim, names.c_str());
}

void draw(SDL_Renderer* r, Game& g, Minimap& map, const Pad* pad = nullptr) {
    const Painter p{r};
    const View view{g.shown_x(g.active())};
    map.refresh(g.world->terrain());
    draw_ground(p, g, view);
    for (int i = 0; i < static_cast<int>(g.looks.size()); ++i)
        if (i != g.active()) draw_vehicle(p, g, i, view, false);
    draw_vehicle(p, g, g.active(), view, true);
    draw_hud(p, g);
    draw_map(p, g, map, view);
    if (pad && pad->wizard.active()) draw_wizard(p, *pad);
}

// ---------------------------------------------------------------- entry points

bool key_action(SDL_Keycode key, Command& out) {
    switch (key) {
        case SDLK_0: case SDLK_n: out = Command::Gear0; return true;
        case SDLK_1: out = Command::Gear1; return true;
        case SDLK_2: out = Command::Gear2; return true;
        case SDLK_3: out = Command::Gear3; return true;
        case SDLK_4: out = Command::Gear4; return true;
        case SDLK_5: out = Command::Gear5; return true;
        case SDLK_6: out = Command::Gear6; return true;
        case SDLK_r: out = Command::Reverse; return true;
        case SDLK_e: out = Command::ShiftUp; return true;
        case SDLK_q: out = Command::ShiftDown; return true;
        case SDLK_m: out = Command::NextMode; return true;
        case SDLK_t: out = Command::ToggleAwd; return true;
        case SDLK_y: out = Command::ToggleLock; return true;
        case SDLK_l: out = Command::ToggleLow; return true;
        case SDLK_b: out = Command::ToggleEngineBrake; return true;
        case SDLK_TAB: out = Command::NextVehicle; return true;
        case SDLK_g: out = Command::Cargo; return true;
        case SDLK_p: out = Command::Pave; return true;
        case SDLK_o: out = Command::Roll; return true;
        case SDLK_BACKSPACE: out = Command::Recover; return true;
        case SDLK_z: out = Command::BladeLower; return true;
        case SDLK_x: out = Command::BladeRaise; return true;
        default: return false;
    }
}

// ---------------------------------------------------------------- telemetry

// Minimal JSON writer: numbers rounded to what a dashboard can show, no allocations per field beyond the string.
struct Json {
    std::string s;
    bool first = true;
    void sep() {
        if (!first) s += ',';
        first = false;
    }
    void key(const char* k) {
        sep();
        s += '"';
        s += k;
        s += "\":";
        first = true;
    }
    void num(double v) {
        sep();
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", std::isfinite(v) ? v : 0.0);
        s += b;
    }
    void boolean(bool v) { sep(), s += v ? "true" : "false"; }
    void str(const char* v) {
        sep();
        s += '"';
        for (const char* c = v; *c; ++c) {
            if (*c == '"' || *c == '\\') s += '\\';
            s += *c;
        }
        s += '"';
    }
    void open(char c) { sep(), s += c, first = true; }
    void close(char c) { s += c, first = false; }
    void field(const char* k, double v) { key(k), num(v); }
    void fixed(const char* k, double v) {  // full precision, for values that keep growing (time)
        key(k), sep();
        char b[32];
        std::snprintf(b, sizeof b, "%.3f", std::isfinite(v) ? v : 0.0);
        s += b;
    }
    void flag(const char* k, bool v) { key(k), boolean(v); }
};

// Everything the active vehicle is doing, for the dashboard at GET / (telemetry_page.h).
std::string telemetry_json(const Game& g, std::uint32_t terrain_version) {
    const Machine& v = g.car();
    const Rig& rig = v.rig;
    const VehicleSpec& s = rig.spec();
    const BodyTelemetry& b = rig.telemetry();
    const Terrain& t = g.world->terrain();
    float max_torque = 0.0f;
    for (const auto& [rpm, nm] : s.engine.curve) max_torque = std::max(max_torque, nm);

    Json j;
    j.open('{');
    j.fixed("t", g.world->time());
    j.field("terrain_version", terrain_version);
    j.key("vehicles"), j.open('[');  // every vehicle, for the 3D view
    for (int i = 0; i < static_cast<int>(g.looks.size()); ++i) {
        const Machine& m = g.world->machine(i);
        const VehicleSpec& ms = m.rig.spec();
        const Rgb c = g.looks[static_cast<std::size_t>(i)].paint;
        char paint[8];
        std::snprintf(paint, sizeof paint, "#%02x%02x%02x", c.r, c.g, c.b);
        j.open('{');
        j.key("label"), j.str(m.label.c_str());
        j.key("paint"), j.str(paint);
        j.flag("active", i == g.active());
        const Pose pose = m.rig.pose(g.shown_x(i));
        j.field("x", g.shown_x(i)), j.field("y", m.rig.y()), j.field("radius", ms.wheel_radius);
        j.field("z", pose.z), j.field("pitch", pose.pitch), j.field("roll", pose.roll);
        j.key("tyre"), j.open('{'), j.field("width", ms.tyre.width), j.field("length", ms.tyre.length), j.close('}');
        j.key("wheels"), j.open('[');
        for (std::size_t k = 0; k < ms.wheels.size(); ++k)
            j.open('{'), j.field("x", ms.wheels[k].x), j.field("y", ms.wheels[k].y), j.field("z", pose.wheel_z[k]), j.close('}');
        j.close(']');
        j.key("blade");
        if (m.has_blade) {
            j.open('{'), j.flag("down", m.blade), j.field("edge", m.blade_edge), j.field("x", g.shown_x(i) + kBladeAhead), j.close('}');
        } else {
            j.sep(), j.s += "null";
        }
        j.close('}');
    }
    j.close(']');
    j.key("vehicle"), j.str(v.label.c_str());
    j.key("mode"), j.str(mode_name(v.box.mode));
    j.field("gear", rig.gear());
    j.field("reverse_gears", static_cast<double>(s.gearbox.reverse.size()));
    j.field("selected", v.box.selected());
    j.flag("shifting", v.box.shifting());
    j.field("x", rig.x());
    j.field("speed", rig.speed());
    j.field("mass", rig.mass());
    j.field("payload", rig.payload());
    j.field("wheel_radius", s.wheel_radius);
    j.key("tyre"), j.open('{'), j.field("width", s.tyre.width), j.field("length", s.tyre.length), j.close('}');
    j.field("rpm", rig.engine_rpm());
    j.field("idle", s.engine.idle_rpm);
    j.field("redline", s.engine.redline_rpm);
    j.field("max_engine_torque", max_torque);
    j.flag("running", rig.engine_running());
    j.flag("grinding", rig.grinding());
    j.key("pedals"), j.open('{');
    j.field("throttle", v.out.throttle), j.field("brake", v.out.brake), j.field("clutch", v.out.clutch);
    j.close('}');
    j.flag("handbrake", v.in.handbrake);
    j.flag("awd", v.in.awd);
    j.flag("lock", v.in.diff_lock);
    j.flag("low", rig.low_range());
    j.flag("engine_brake", v.in.engine_brake);
    j.field("engine_torque", b.engine_torque);
    j.field("clutch_torque", b.clutch_torque);
    j.field("clutch_capacity", b.clutch_capacity);
    j.field("clutch_slip_rpm", b.clutch_slip_rpm);
    j.flag("clutch_locked", rig.clutch_locked());
    j.field("fuel_rate_lph", b.fuel_rate * 3600.0f / 0.84f);
    j.field("fuel_used_l", rig.fuel_kg() / 0.84f);
    j.key("forces"), j.open('{');
    j.field("traction", b.traction), j.field("resistance", b.resistance), j.field("grade", b.grade);
    j.field("belly", b.belly), j.field("external", b.external);
    j.close('}');
    j.field("sinkage", rig.mean_sinkage());
    j.field("clearance", s.clearance);

    j.key("wheels"), j.open('[');
    const auto wt = rig.wheel_telemetry();
    for (std::size_t i = 0; i < wt.size(); ++i) {
        const WheelTelemetry& w = wt[i];
        j.open('{');
        j.field("x", s.wheels[i].x), j.field("y", s.wheels[i].y), j.field("load", w.load), j.field("sink", w.sinkage);
        j.field("rut", w.rut), j.field("gravel", w.gravel), j.field("soft", w.softness), j.field("slip", w.slip);
        j.field("traction", w.traction), j.field("max", w.max_traction), j.field("resist", w.resistance);
        j.field("torque", w.drive_torque), j.field("brake", w.brake), j.field("omega", w.omega);
        j.close('}');
    }
    j.close(']');

    j.key("blade");
    if (v.has_blade) {
        j.open('{');
        j.flag("down", v.blade), j.field("layer", v.blade_layer), j.field("edge", v.blade_edge);
        j.field("pile", v.blade_pile), j.field("force", v.blade_force), j.field("x", rig.x() + kBladeAhead);
        j.close('}');
    } else {
        j.sep(), j.s += "null";
    }

    // The ground along the left wheel track, as the side view draws it, 15 m either side.
    const float cs = t.cell_size(), y = rig.y() - 1.0f, x0 = std::floor((rig.x() - 15.0f) / cs) * cs;
    const int n = static_cast<int>(30.0f / cs);
    auto series = [&](const char* name, auto get) {
        j.key(name), j.open('[');
        for (int i = 0; i < n; ++i) j.num(get(t.at(x0 + (i + 0.5f) * cs, y)));
        j.close(']');
    };
    j.key("ground"), j.open('{');
    j.field("x0", x0 + 0.5f * cs), j.field("dx", cs);
    series("hard", [](const Cell& c) { return c.hard; });
    series("mud", [](const Cell& c) { return c.mud_top(); });
    series("base", [](const Cell& c) { return c.mud_top() + c.gravel; });
    series("top", [](const Cell& c) { return c.surface(); });
    series("paved", [](const Cell& c) { return static_cast<int>(c.paving); });
    j.close('}');
    j.close('}');
    return j.s;
}

// The whole ground for the 3D view (view3d_page.h), little-endian: "INCT", u32 version, u16 nx, u16 ny, f32 cell
// size, then per cell in millimetres: i16 surface, u16 gravel, u16 loose gravel on asphalt, u16 rut, u16 mud depth,
// and u8 paving (0 none, 1 laid, 2 rolled). About 11 bytes a cell: 380 kB for the playground.
std::string terrain_blob(const Terrain& t, std::uint32_t version) {
    const std::size_t n = static_cast<std::size_t>(t.nx()) * t.ny();
    std::string out(16 + 11 * n, '\0');
    char* p = out.data();
    auto put = [&](std::size_t at, const void* v, std::size_t len) { std::memcpy(p + at, v, len); };
    put(0, "INCT", 4);
    const auto nx = static_cast<std::uint16_t>(t.nx()), ny = static_cast<std::uint16_t>(t.ny());
    const float cs = t.cell_size();
    put(4, &version, 4), put(8, &nx, 2), put(10, &ny, 2), put(12, &cs, 4);
    auto mm = [](float m) { return static_cast<long>(std::lround(m * 1000.0f)); };
    for (std::size_t i = 0; i < n; ++i) {
        const Cell& c = t.cell(static_cast<int>(i % t.nx()), static_cast<int>(i / t.nx()));
        const auto top = static_cast<std::int16_t>(std::clamp(mm(c.surface()), -32768L, 32767L));
        const std::uint16_t layers[4] = {static_cast<std::uint16_t>(std::clamp(mm(c.gravel), 0L, 65535L)),
                                         static_cast<std::uint16_t>(std::clamp(mm(c.loose), 0L, 65535L)),
                                         static_cast<std::uint16_t>(std::clamp(mm(c.rut), 0L, 65535L)),
                                         static_cast<std::uint16_t>(std::clamp(mm(c.soft), 0L, 65535L))};
        put(16 + 2 * i, &top, 2);
        for (std::size_t k = 0; k < 4; ++k) put(16 + 2 * n * (k + 1) + 2 * i, &layers[k], 2);
        const auto paving = static_cast<std::uint8_t>(c.paving);
        put(16 + 10 * n + i, &paving, 1);
    }
    return out;
}

struct AudioOut {
    SDL_AudioDeviceID device = 0;
    Synth synth{48000.0f};
    SoundState state;
    bool muted = false;
};

void audio_callback(void* user, Uint8* stream, int len) {
    auto* a = static_cast<AudioOut*>(user);
    a->synth.render(reinterpret_cast<float*>(stream), len / static_cast<int>(sizeof(float)), a->muted ? SoundState{} : a->state);
}

int play(int telemetry_port, bool telemetry_lan, bool fullscreen) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // The game draws at kW x kH and SDL scales that to whatever size the window is, keeping the aspect ratio.
    // Linear filtering, so a 1.5x scale (1920x1080) does not leave uneven pixel widths.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    const Uint32 flags = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_Window* window = SDL_CreateWindow("Incatramato - core loop prototype", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW, kH, flags);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    // SDL2 starts with text input on, which hands held keys to the desktop's input method. On KDE Plasma
    // that is the accent popup: holding S (or E, A, ...) reaches us as an instant press and release, so the
    // brake drops out. The game never takes typed text.
    SDL_StopTextInput();
    if (!renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, kW, kH);

    AudioOut audio;  // the game runs silent if no audio device opens
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 1;
        want.samples = 512;
        want.callback = audio_callback;
        want.userdata = &audio;
        audio.device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audio.device) SDL_PauseAudioDevice(audio.device, 0);
    }

    Pad pad;
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    pad.devices.refresh();
    pad.map.load(kBindingsFile);

    auto game = std::make_unique<Game>();
    Minimap map;
    map.rebuild(renderer, game->world->terrain());

    telemetry::Server telemetry;
    if (telemetry_port < 0) {
        // --no-telemetry
    } else if (telemetry.start(telemetry_port, telemetry_lan)) {
        std::printf("telemetry: http://%s:%d/  (open it on a second screen)\n", telemetry_lan ? "<this machine>" : "localhost",
                    telemetry.port());
        std::fflush(stdout);
        char buf[64];
        std::snprintf(buf, sizeof buf, "TELEMETRY: HTTP://LOCALHOST:%d", telemetry.port());
        game->say(buf);
        game->message_time = 6.0f;
    } else {
        std::fprintf(stderr, "telemetry: could not listen on port %d, running without it\n", telemetry_port);
    }
    Uint64 last_publish = 0, last_ground = 0;
    std::uint32_t ground_sent = ~0u;
    Uint64 last = SDL_GetTicks64();
    Uint32 brake_tap = 0;  // time of the last brake key press, for the double tap
    for (bool running = true; running;) {
        for (SDL_Event ev; SDL_PollEvent(&ev);) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_JOYDEVICEADDED || ev.type == SDL_JOYDEVICEREMOVED) {
                pad.devices.refresh();
                if (pad.wizard.active()) {  // its picture of the axes at rest is per device index, now stale
                    pad.wizard.cancel();
                    game->say("CONTROLLERS CHANGED: SETUP CANCELLED, PRESS F3 AGAIN");
                }
            }
            if (ev.type != SDL_KEYDOWN) continue;
            const SDL_Keycode key = ev.key.keysym.sym;
            const SDL_Scancode code = ev.key.keysym.scancode;
            if ((code == SDL_SCANCODE_S || code == SDL_SCANCODE_DOWN) && !ev.key.repeat && !pad.wizard.active()) {
                if (brake_tap != 0 && ev.key.timestamp - brake_tap <= kDoubleTapMs) {
                    send(*game, Command::ToggleHandbrake);
                    brake_tap = 0;
                } else {
                    brake_tap = ev.key.timestamp;
                }
            }
            Command a;
            if (key == SDLK_F3) {
                if (pad.wizard.active()) pad.wizard.cancel();
                else if (pad.devices.sticks().empty()) game->say("NO CONTROLLER CONNECTED");
                else pad.wizard.start(pad.devices);
            } else if (key == SDLK_ESCAPE && pad.wizard.active()) {
                pad.wizard.skip(pad.map, pad.devices);
                if (!pad.wizard.active()) pad.map.save(kBindingsFile);
            } else if (key == SDLK_ESCAPE) {
                running = false;
            } else if (key == SDLK_F11) {
                const bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (key == SDLK_F1) {
                audio.muted = !audio.muted;
                game->say(audio.muted ? "SOUND OFF" : "SOUND ON");
            } else if (key == SDLK_F5) {
                game = std::make_unique<Game>();
                map.rebuild(renderer, game->world->terrain());
            } else if (key_action(key, a) && (!ev.key.repeat || a == Command::Pave || a == Command::Roll)) {
                send(*game, a);
            }
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        Held held;
        held.throttle = keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP];
        held.brake = keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN];
        held.clutch = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        held.starter = keys[SDL_SCANCODE_I];
        held.cargo = keys[SDL_SCANCODE_G];
        const Uint64 now = SDL_GetTicks64();
        const float frame_dt = std::min((now - last) / 1000.0f, 0.05f);
        last = now;
        if (pad.wizard.active()) {
            if (pad.wizard.poll(pad.map, pad.devices, frame_dt)) {
                pad.map.save(kBindingsFile);
                game->say("CONTROLLER SAVED TO INPUT.CFG");
            }
        } else {
            poll_pad(pad, *game, held);
        }
        update(*game, held, frame_dt);
        if (telemetry.running() && now - last_publish >= 33) {  // 30 Hz is plenty; the stream sends 20
            telemetry.publish(telemetry_json(*game, map.version));
            last_publish = now;
        }
        if (telemetry.running() && map.version != ground_sent && now - last_ground >= 250) {  // the 3D view's ground
            telemetry.publish_resource("/terrain.bin", "application/octet-stream", terrain_blob(game->world->terrain(), map.version));
            ground_sent = map.version;
            last_ground = now;
        }
        if (audio.device) {
            SDL_LockAudioDevice(audio.device);
            audio.state = sound_state(*game, held);
            SDL_UnlockAudioDevice(audio.device);
        }
        draw(renderer, *game, map, &pad);
        SDL_RenderPresent(renderer);
        // Present normally waits for the display; where vsync is not honoured the loop would spin flat out and
        // take a whole core, so never run faster than about 144 frames a second.
        const Uint64 spent = SDL_GetTicks64() - now;
        if (spent < 7) SDL_Delay(static_cast<Uint32>(7 - spent));
    }
    if (audio.device) SDL_CloseAudioDevice(audio.device);
    SDL_Quit();
    return 0;
}

void write_wav(const std::string& path, const std::vector<float>& samples, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 2);
    u16(2);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (float s : samples) u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f)));
    std::fclose(f);
}

// Binds a virtual wheel, pedals and shifter through the wizard, then drives the game with it.
// Returns the number of failed checks, or 0 if this SDL build has no virtual joysticks.
#ifndef _WIN32
// One HTTP GET against localhost; returns the whole response (headers and body), reading at most `limit` bytes.
std::string http_get(int port, const char* path, std::size_t limit = 1 << 20) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out;
    timeval timeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
        const std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ::send(fd, req.data(), req.size(), 0);
        char buf[4096];
        for (ssize_t n; out.size() < limit && (n = ::recv(fd, buf, sizeof buf, 0)) > 0;) out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// The telemetry server answers on all three routes with what the game published.
int telemetry_test(const Game& g) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL telemetry: %s\n", what);
            ++failures;
        }
    };
    telemetry::Server server;
    check(server.start(0, false), "server starts on a free port");
    const std::string json = telemetry_json(g, 0);
    server.publish(json);
    check(json.front() == '{' && json.back() == '}', "snapshot is a JSON object");
    check(json.find("\"wheels\":[{") != std::string::npos && json.find("\"ground\":{") != std::string::npos, "snapshot has wheels and ground");
    const std::string snap = http_get(server.port(), "/snapshot");
    check(snap.rfind("HTTP/1.1 200", 0) == 0 && snap.size() >= json.size() && snap.compare(snap.size() - json.size(), json.size(), json) == 0,
          "GET /snapshot returns the published JSON");
    const std::string page = http_get(server.port(), "/");
    check(page.find("text/html") != std::string::npos && page.find("new EventSource(\"/stream\")") != std::string::npos, "GET / serves the dashboard");
    const std::string stream = http_get(server.port(), "/stream", 2048);
    check(stream.find("text/event-stream") != std::string::npos && stream.find("data: {") != std::string::npos, "GET /stream sends snapshots");
    check(http_get(server.port(), "/nothing").rfind("HTTP/1.1 404", 0) == 0, "unknown paths are 404");
    const std::string page3d = http_get(server.port(), "/3d");
    check(page3d.find("text/html") != std::string::npos && page3d.find("webgl2") != std::string::npos, "GET /3d serves the 3D view");
    const Terrain& t = g.world->terrain();
    server.publish_resource("/terrain.bin", "application/octet-stream", terrain_blob(t, 7));
    const std::string bin = http_get(server.port(), "/terrain.bin");
    const std::size_t body = bin.find("\r\n\r\n") + 4;
    check(bin.size() - body == 16 + 11 * static_cast<std::size_t>(t.nx()) * t.ny() && bin.compare(body, 4, "INCT") == 0,
          "GET /terrain.bin returns the whole ground");
    server.stop();
    std::printf("telemetry test: %s\n", failures ? "FAILED" : "passed");
    return failures;
}
#endif

int controller_test(SDL_Renderer* renderer, SDL_Surface* surface, Game& g, Minimap& map, const std::string& prefix) {
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0 || SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_WHEEL, 3, 12, 0) < 0) {
        std::printf("controller test skipped: no virtual joystick support\n");
        return 0;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL controller: %s\n", what);
            ++failures;
        }
    };
    Pad pad;
    pad.devices.refresh();
    SDL_Joystick* j = pad.devices.sticks().back();
    auto axis = [&](int a, int v) {
        SDL_JoystickSetVirtualAxis(j, a, static_cast<Sint16>(v));
        SDL_JoystickUpdate();
    };
    auto button = [&](int b, bool down) {
        SDL_JoystickSetVirtualButton(j, b, down);
        SDL_JoystickUpdate();
    };
    auto wait = [&](float seconds) {
        for (int i = 0, n = static_cast<int>(seconds * 60.0f); i < n; ++i) pad.wizard.poll(pad.map, pad.devices, 1.0f / 60.0f);
    };
    // Axis 0: throttle pedal resting at the bottom of its range. Axis 1: clutch pedal wired the other way up.
    // Axis 2: a stick, centred; pulling it back is the brake. Buttons 0-6: shifter gates 1-6 and reverse.
    axis(0, -32768);
    axis(1, 32767);
    axis(2, 0);
    pad.wizard.start(pad.devices);
    wait(0.5f);
    draw(renderer, g, map, &pad);
    SDL_SaveBMP(surface, (prefix + "0.bmp").c_str());
    axis(0, 32767), wait(0.1f), axis(0, -32768), wait(0.5f);  // throttle
    axis(2, -32768), wait(0.1f), axis(2, 0), wait(0.5f);      // brake
    axis(1, -32768), wait(0.1f), axis(1, 32767), wait(0.5f);  // clutch
    pad.wizard.skip(pad.map, pad.devices);                    // shift up
    pad.wizard.skip(pad.map, pad.devices);                    // shift down
    wait(0.5f);
    for (int b = 0; b < 7; ++b) button(b, true), wait(0.1f), button(b, false), wait(0.5f);
    check(pad.wizard.current() == input::Mode, "wizard reached the buttons after the shifter gates");
    while (pad.wizard.active()) pad.wizard.skip(pad.map, pad.devices);

    axis(0, 0);
    check(std::abs(pad.map.axis(input::Throttle, pad.devices) - 0.5f) < 0.03f, "throttle pedal at half travel reads 0.5");
    axis(2, -16384);
    check(std::abs(pad.map.axis(input::Brake, pad.devices) - 0.5f) < 0.03f, "stick pulled half back reads 0.5 brake");
    axis(2, 20000);
    check(pad.map.axis(input::Brake, pad.devices) == 0.0f, "stick pushed forward is no brake");
    check(pad.map.axis(input::Clutch, pad.devices) == 0.0f, "inverted clutch pedal reads 0 at rest");
    axis(1, -32768);
    check(pad.map.axis(input::Clutch, pad.devices) == 1.0f, "inverted clutch pedal reads 1 on the floor");

    const std::string file = prefix + "input.cfg";
    input::Map loaded;
    check(pad.map.save(file.c_str()) && loaded.load(file.c_str()) && loaded.b == pad.map.b, "bindings survive save and load");

    // Drive with it: manual box, clutch down, lever into the first gate, feed the clutch out with some throttle.
    while (g.car().box.mode != ShiftMode::Manual) send(g, Command::NextMode);
    auto frames = [&](float seconds) {
        for (int i = 0, n = static_cast<int>(seconds * 60.0f); i < n; ++i) {
            Held h;
            poll_pad(pad, g, h);
            update(g, h, 1.0f / 60.0f);
        }
    };
    axis(0, -32768), axis(2, 0), axis(1, -32768);
    button(0, true);
    frames(0.5f);
    check(g.car().rig.gear() == 1, "lever in the first gate with the clutch down engages first");
    axis(0, 0);
    for (int i = 0; i <= 60; ++i) axis(1, -32768 + i * 1092), frames(1.0f / 60.0f);
    frames(3.0f);
    check(g.car().rig.speed() > 1.0f && g.car().rig.engine_running(), "truck pulls away on the pedals");
    button(0, false);
    frames(0.2f);
    check(g.car().rig.gear() == 0, "lever out of the gate is neutral");
    std::printf("controller test: %s\n", failures ? "FAILED" : "passed");
    return failures;
}

// Runs a scripted session without a window and writes screenshots and the soundtrack: a smoke test for the app.
int selftest(const std::string& prefix) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    auto game = std::make_unique<Game>();
    Game& g = *game;
    Minimap map;
    map.rebuild(renderer, g.world->terrain());
    {
        auto scratch = std::make_unique<Game>();  // the controller test drives its own world
        if (controller_test(renderer, surface, *scratch, map, prefix) != 0) return 1;
        map.rebuild(renderer, g.world->terrain());
    }

    constexpr int kWavRate = 16000;
    Synth synth(kWavRate);
    std::vector<float> sound;
    float clock = 0.0f;
    int shot_no = 0;
    auto run = [&](Held held, float seconds) {
        for (int i = 0, n = static_cast<int>(seconds * 60.0f); i < n; ++i) {
            update(g, held, 1.0f / 60.0f);
            const std::size_t at = sound.size();
            sound.resize(at + kWavRate / 60);
            synth.render(sound.data() + at, kWavRate / 60, sound_state(g, held));
            clock += 1.0f / 60.0f;
        }
    };
    auto shot = [&](const char* what) {
        draw(renderer, g, map);
        SDL_RenderPresent(renderer);
        const std::string path = prefix + std::to_string(++shot_no) + ".bmp";
        SDL_SaveBMP(surface, path.c_str());
        const Rig& rig = g.car().rig;
        std::printf("t=%5.1f s  %s: %s | x %.1f m, %.1f km/h, gear %d, %.0f rpm, sink %.0f cm, %s\n", clock, path.c_str(), what, rig.x(),
                    rig.speed() * 3.6f, rig.gear(), rig.engine_rpm(), rig.mean_sinkage() * 100.0f,
                    rig.engine_running() ? "running" : "stalled");
    };
    Held gas;
    gas.throttle = true;
    Held clutch;
    clutch.clutch = true;
    auto set_mode = [&](ShiftMode m) {
        while (g.car().box.mode != m) send(g, Command::NextMode);
    };

    run({}, 3.0f);  // idle
    // 1. Manual box: pushing the lever at first with the clutch up grinds.
    set_mode(ShiftMode::Manual);
    send(g, Command::Gear1);
    run({}, 0.1f);
    shot("manual, first gear without the clutch");
    // 2. Clutch down, first, feed it in with throttle, then into the mud.
    run(clutch, 0.4f);
    run(gas, 30.0f);
    shot("stock truck, manual first gear, driven into the mud");
    // 3. Offroad truck on the automatic with AWD and locks.
    send(g, Command::NextVehicle);
    send(g, Command::ToggleAwd);
    send(g, Command::ToggleLock);
    send(g, Command::Gear1);
    run(gas, 14.0f);
    shot("offroad truck, automatic, AWD + locks, in the mud");
    run(gas, 14.0f);
    shot("offroad truck, later");
    // 4. Gravel: five loads tipped along the mud, then the dozer grades towards them.
    for (int i = 0; i < 5; ++i) g.world->terrain().dump_gravel(64.0f + 8.0f * i, g.world->course().lane_y, kLoad);
    send(g, Command::NextVehicle);
    send(g, Command::Cargo);
    send(g, Command::Gear1);
    run(gas, 16.0f);
    shot("dozer grading, piles ahead");
    run(gas, 30.0f);
    // 5. Pave and roll the first half, then send the stock truck across.
    g.world->terrain().pave({58.0f, 3.5f, 80.0f, 8.5f}, kMinBase);
    g.world->terrain().roll({58.0f, 3.5f, 80.0f, 8.5f});
    send(g, Command::NextVehicle);
    send(g, Command::Recover);
    set_mode(ShiftMode::Automatic);
    send(g, Command::Gear1);
    run(gas, 9.0f);
    shot("stock truck, automatic, on the new road");
    run(gas, 12.0f);
    shot("stock truck, later");
    std::printf("%s\n", g.last_trip.empty() ? "no trip completed" : g.last_trip.c_str());
    write_wav(prefix + "sound.wav", sound, kWavRate);
#ifndef _WIN32
    if (telemetry_test(g) != 0) return 1;
#endif
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    if (argc == 3 && std::string(argv[1]) == "--selftest") return selftest(argv[2]);
    int port = 8787;
    bool lan = false, fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--telemetry-port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--telemetry-lan") lan = true;  // let a tablet or another computer open the dashboard
        else if (arg == "--no-telemetry") port = -1;
        else if (arg == "--fullscreen") fullscreen = true;
        else {
            std::fprintf(stderr, "usage: play [--fullscreen] [--telemetry-port N] [--telemetry-lan] [--no-telemetry] | --selftest PREFIX\n");
            return 2;
        }
    }
    return play(port, lan, fullscreen);
}
