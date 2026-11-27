#include "inc/tuning.h"

#include <boost/json.hpp>

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace inc {
namespace {

namespace json = boost::json;

// Thrown for anything that makes the file unusable; becomes TuningResult::error.
struct Bad {
    std::string what;
};

// Reads values out of one JSON object into the tuning, remembering which keys it knew so the rest can be
// reported as probable typos.
class Reader {
public:
    Reader(const json::value& v, std::string path, std::vector<std::string>& warnings)
        : path_(std::move(path)), warnings_(warnings) {
        if (!v.is_object()) throw Bad{path_ + " must be an object"};
        obj_ = &v.as_object();
    }
    ~Reader() noexcept(false) {
        if (std::uncaught_exceptions() > 0) return;
        for (const auto& kv : *obj_) {
            const std::string key(kv.key());
            bool seen = false;
            for (const std::string& k : known_) seen |= k == key;
            if (!seen) warnings_.push_back("unknown key " + path_ + "." + key + " (ignored)");
        }
    }

    const json::value* find(const char* key) {
        known_.emplace_back(key);
        return obj_->if_contains(key);
    }
    void num(const char* key, float& out) {
        if (const json::value* v = find(key)) out = number(*v, at(key));
    }
    void flag(const char* key, bool& out) {
        if (const json::value* v = find(key)) {
            if (!v->is_bool()) throw Bad{at(key) + " must be true or false"};
            out = v->as_bool();
        }
    }
    void numbers(const char* key, std::vector<float>& out) {
        if (const json::value* v = find(key)) {
            if (!v->is_array()) throw Bad{at(key) + " must be a list of numbers"};
            out.clear();
            for (std::size_t i = 0; i < v->as_array().size(); ++i)
                out.push_back(number(v->as_array()[i], at(key) + "[" + std::to_string(i) + "]"));
        }
    }
    std::string at(const char* key) const { return path_ + "." + key; }
    std::vector<std::string>& warnings() { return warnings_; }

    static float number(const json::value& v, const std::string& where) {
        if (!v.is_number()) throw Bad{where + " must be a number"};
        const double d = v.to_number<double>();
        if (!std::isfinite(d)) throw Bad{where + " must be finite"};
        return static_cast<float>(d);
    }

private:
    const json::object* obj_;
    std::string path_;
    std::vector<std::string>& warnings_;
    std::vector<std::string> known_;
};

void positive(float v, const std::string& where) {
    if (!(v > 0.0f)) throw Bad{where + " must be more than 0"};
}

void read_soil(const json::value& v, SoilParams& s, std::vector<std::string>& w) {
    Reader r(v, "soil", w);
    r.num("n", s.n), r.num("k_c", s.k_c), r.num("k_phi", s.k_phi), r.num("cohesion", s.cohesion);
    r.num("tan_phi", s.tan_phi), r.num("shear_k", s.shear_k), r.num("dig_rate", s.dig_rate), r.num("rut_drag", s.rut_drag);
    r.num("viscous", s.viscous);
    positive(s.n, "soil.n"), positive(s.shear_k, "soil.shear_k");
}

void read_surfaces(const json::value& v, SoilParams& s, std::vector<std::string>& w) {
    Reader r(v, "surfaces", w);
    r.num("crr_firm", s.crr_firm), r.num("crr_gravel", s.crr_gravel), r.num("crr_laid", s.crr_laid);
    r.num("crr_rolled", s.crr_rolled), r.num("laid_grip", s.laid_grip), r.num("hard_slip_gain", s.hard_slip_gain);
    r.num("gravel_full_cover", s.gravel_full_cover);
    positive(s.gravel_full_cover, "surfaces.gravel_full_cover");
}

void read_tyre(const json::value& v, const std::string& name, Tyre& t, std::vector<std::string>& w) {
    const std::string path = "tyres." + name;
    Reader r(v, path, w);
    r.num("width", t.width), r.num("length", t.length), r.num("soft_grip", t.soft_grip), r.num("crr_scale", t.crr_scale);
    if (const json::value* g = r.find("grip")) {
        Reader gr(*g, path + ".grip", w);
        gr.num("firm", t.grip.firm), gr.num("wet", t.grip.wet), gr.num("gravel", t.grip.gravel), gr.num("asphalt", t.grip.asphalt);
    }
    positive(t.width, path + ".width"), positive(t.length, path + ".length");
}

void read_vehicle(const json::value& v, const std::string& name, VehicleSpec& s, std::vector<std::string>& w) {
    const std::string path = "vehicles." + name;
    Reader r(v, path, w);
    r.num("mass", s.mass), r.num("wheel_radius", s.wheel_radius), r.num("wheel_inertia", s.wheel_inertia);
    r.num("clearance", s.clearance), r.num("brake_torque", s.brake_torque), r.num("clutch_max_torque", s.clutch.max_torque);
    if (const json::value* e = r.find("engine")) {
        Engine& en = s.engine;
        Reader er(*e, path + ".engine", w);
        if (const json::value* c = er.find("torque_curve")) {
            const std::string where = er.at("torque_curve");
            if (!c->is_array() || c->as_array().size() < 2) throw Bad{where + " must be a list of at least two [rpm, N*m] pairs"};
            en.curve.clear();
            for (std::size_t i = 0; i < c->as_array().size(); ++i) {
                const json::value& p = c->as_array()[i];
                const std::string at = where + "[" + std::to_string(i) + "]";
                if (!p.is_array() || p.as_array().size() != 2) throw Bad{at + " must be an [rpm, N*m] pair"};
                en.curve.emplace_back(Reader::number(p.as_array()[0], at), Reader::number(p.as_array()[1], at));
                if (i > 0 && !(en.curve[i].first > en.curve[i - 1].first)) throw Bad{at + ": rpm must rise along the curve"};
            }
        }
        er.num("idle_rpm", en.idle_rpm), er.num("redline_rpm", en.redline_rpm), er.num("stall_rpm", en.stall_rpm);
        er.num("inertia", en.inertia), er.num("friction", en.friction), er.num("exhaust_brake", en.exhaust_brake);
        er.num("starter_torque", en.starter_torque), er.num("governor_band", en.governor_band);
        if (!(en.stall_rpm < en.idle_rpm && en.idle_rpm < en.redline_rpm)) throw Bad{path + ".engine: needs stall_rpm < idle_rpm < redline_rpm"};
        positive(en.inertia, path + ".engine.inertia"), positive(en.governor_band, path + ".engine.governor_band");
    }
    if (const json::value* g = r.find("gearbox")) {
        Reader gr(*g, path + ".gearbox", w);
        gr.numbers("forward", s.gearbox.forward), gr.numbers("reverse", s.gearbox.reverse);
        gr.num("final_drive", s.gearbox.final_drive), gr.num("low_range", s.gearbox.low_range);
        if (s.gearbox.forward.empty()) throw Bad{path + ".gearbox.forward needs at least one gear"};
    }
    if (const json::value* ws = r.find("wheels")) {
        // The layout can move and the loads can shift, but the number of wheels is fixed by the driveline in code.
        const std::string where = r.at("wheels");
        if (!ws->is_array() || ws->as_array().size() != s.wheels.size())
            throw Bad{where + " must list exactly " + std::to_string(s.wheels.size()) + " wheels (the driveline fixes the count)"};
        for (std::size_t i = 0; i < s.wheels.size(); ++i) {
            WheelSpec& wh = s.wheels[i];
            Reader wr(ws->as_array()[i], where + "[" + std::to_string(i) + "]", w);
            wr.num("x", wh.x), wr.num("y", wh.y), wr.num("load", wh.load_fraction), wr.num("payload", wh.payload_fraction);
            wr.flag("handbrake", wh.handbrake);
        }
    }
    positive(s.mass, path + ".mass"), positive(s.wheel_radius, path + ".wheel_radius");
    positive(s.wheel_inertia, path + ".wheel_inertia"), positive(s.clutch.max_torque, path + ".clutch_max_torque");
}

void read_shifting(const json::value& v, ShiftParams& p, std::vector<std::string>& w) {
    Reader r(v, "shifting", w);
    r.num("open_time", p.open_time), r.num("max_sync_time", p.max_sync_time), r.num("close_time", p.close_time);
    r.num("min_shift_gap", p.min_shift_gap), r.num("up_base", p.up_base), r.num("up_per_throttle", p.up_per_throttle);
    r.num("down_base", p.down_base), r.num("down_per_throttle", p.down_per_throttle), r.num("launch_revs", p.launch_revs);
    r.num("launch_span", p.launch_span), r.num("launch_touch", p.launch_touch), r.num("launch_touch_throttle", p.launch_touch_throttle);
    positive(p.close_time, "shifting.close_time"), positive(p.launch_span, "shifting.launch_span");
    positive(p.launch_touch_throttle, "shifting.launch_touch_throttle");
}

void read_rules(const json::value& v, Rules& x, std::vector<std::string>& w) {
    Reader r(v, "rules", w);
    r.num("load", x.load), r.num("min_base", x.min_base), r.num("tip_ahead", x.tip_ahead), r.num("tip_rate", x.tip_rate);
    r.num("tip_width", x.tip_width), r.num("tip_clearance", x.tip_clearance), r.num("blade_ahead", x.blade_ahead);
    r.num("blade_width", x.blade_width), r.num("gravel_density", x.gravel_density), r.num("push_friction", x.push_friction);
    r.num("settle_time", x.settle_time);
    positive(x.blade_width, "rules.blade_width"), positive(x.tip_rate, "rules.tip_rate");
}

void read_assists(const json::value& v, Assists& a, std::vector<std::string>& w) {
    Reader r(v, "assists", w);
    r.flag("blade_grade_control", a.blade_grade_control);
}

void read_pedals(const json::value& v, Pedals& p, std::vector<std::string>& w) {
    Reader r(v, "pedals", w);
    for (auto [key, curve] : {std::pair<const char*, PedalCurve*>{"throttle", &p.throttle}, {"brake", &p.brake}, {"clutch", &p.clutch}})
        if (const json::value* c = r.find(key)) {
            Reader cr(*c, r.at(key), w);
            cr.num("deadzone", curve->deadzone), cr.num("gamma", curve->gamma);
            positive(curve->gamma, cr.at("gamma"));
            if (!(curve->deadzone >= 0.0f && curve->deadzone < 0.5f)) throw Bad{cr.at("deadzone") + " must be from 0 to 0.5"};
        }
    if (const json::value* k = r.find("keyboard")) {
        Reader kr(*k, "pedals.keyboard", w);
        kr.num("throttle_up", p.key_throttle_up), kr.num("throttle_down", p.key_throttle_down), kr.num("brake_up", p.key_brake_up);
        kr.num("brake_down", p.key_brake_down), kr.num("clutch_down", p.key_clutch_down), kr.num("clutch_up", p.key_clutch_up);
    }
}

}  // namespace

Tuning Tuning::defaults() {
    Tuning t;
    t.tyres["highway"] = presets::highway_tyre();
    t.tyres["mud"] = presets::mud_tyre();
    t.vehicles["truck_6x6"] = presets::truck_6x6(presets::highway_tyre());
    t.vehicles["dozer"] = presets::dozer();
    t.tyres["track"] = t.vehicles["dozer"].tyre;
    return t;
}

VehicleSpec Tuning::spec(const std::string& vehicle, const std::string& tyre) const {
    VehicleSpec s = vehicles.at(vehicle);
    s.tyre = tyres.at(tyre);
    return s;
}

TuningResult load_tuning_text(const std::string& text, Tuning& t, const std::string& name) {
    TuningResult result;
    json::parse_options opt;
    opt.allow_comments = true;
    opt.allow_trailing_commas = true;
    json::stream_parser parser({}, opt);
    boost::system::error_code ec;
    const std::size_t used = parser.write_some(text.data(), text.size(), ec);
    if (!ec) parser.finish(ec);
    if (ec) {  // where it stopped is where the mistake is
        const std::size_t at = std::min(used, text.size());
        const long line = 1 + std::count(text.begin(), text.begin() + static_cast<long>(at), '\n');
        const std::size_t nl = text.rfind('\n', at == 0 ? 0 : at - 1);
        const std::size_t col = nl == std::string::npos ? at + 1 : at - nl;
        result.ok = false;
        result.error = name + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + ec.message();
        return result;
    }
    const json::value root = parser.release();

    Tuning next = t;  // all or nothing
    try {
        Reader r(root, name, result.warnings);
        if (const json::value* v = r.find("soil")) read_soil(*v, next.soil, result.warnings);
        if (const json::value* v = r.find("surfaces")) read_surfaces(*v, next.soil, result.warnings);
        if (const json::value* v = r.find("tyres")) {
            Reader tr(*v, "tyres", result.warnings);
            for (auto& [key, tyre] : next.tyres)
                if (const json::value* tv = tr.find(key.c_str())) read_tyre(*tv, key, tyre, result.warnings);
        }
        if (const json::value* v = r.find("vehicles")) {
            Reader vr(*v, "vehicles", result.warnings);
            for (auto& [key, spec] : next.vehicles)
                if (const json::value* vv = vr.find(key.c_str())) read_vehicle(*vv, key, spec, result.warnings);
        }
        if (const json::value* v = r.find("shifting")) read_shifting(*v, next.shift, result.warnings);
        if (const json::value* v = r.find("rules")) read_rules(*v, next.rules, result.warnings);
        if (const json::value* v = r.find("assists")) read_assists(*v, next.assists, result.warnings);
        if (const json::value* v = r.find("pedals")) read_pedals(*v, next.pedals, result.warnings);
    } catch (const Bad& bad) {
        result.ok = false;
        result.error = name + ": " + bad.what;
        result.warnings.clear();
        return result;
    }
    t = std::move(next);
    return result;
}

TuningResult load_tuning(const std::string& path, Tuning& t) {
    std::ifstream in(path);
    if (!in) {
        TuningResult r;
        r.ok = false;
        r.error = path + ": cannot open";
        return r;
    }
    std::stringstream text;
    text << in.rdbuf();
    return load_tuning_text(text.str(), t, path);
}

}  // namespace inc
