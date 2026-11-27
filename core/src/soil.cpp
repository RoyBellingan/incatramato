#include "inc/soil.h"

#include <algorithm>
#include <cmath>

namespace inc {
namespace {

float modulus(const SoilParams& s, float width) { return s.k_c / width + s.k_phi; }

// Placeholder friction and rolling coefficients for hard surfaces.
struct Hard {
    float mu, crr;
};
constexpr Hard kFirmDirt{0.55f, 0.035f};
constexpr Hard kBottomedOut{0.40f, 0.035f};  // firm ground under a layer of mud
constexpr Hard kGravel{0.60f, 0.025f};
constexpr Hard kAsphaltLaid{0.65f, 0.020f};
constexpr Hard kAsphaltRolled{0.80f, 0.008f};
constexpr float kHardSlipGain = 12.0f;
constexpr float kGravelFullCover = 0.15f;  // gravel thickness at which the wheel no longer touches mud [m]

Contact hard_contact(Hard h, const Tyre& t, float load, bool diggable) {
    return {h.mu * t.hard_grip * load, kHardSlipGain, h.crr * t.crr_scale * load, 0.0f, diggable};
}

}  // namespace

float equilibrium_sinkage(const Cell& c, const SoilParams& s, const Tyre& t, float load) {
    if (c.paving != Paving::None || c.soft <= 0.0f) return 0.0f;
    const float w = t.width + c.gravel, l = t.length + c.gravel;
    const float pressure = load / (w * l);
    return std::min(std::pow(pressure / modulus(s, w), 1.0f / s.n), c.soft);
}

float compaction_resistance(const Cell& ahead, const SoilParams& s, const Tyre& t, float load) {
    const float z = equilibrium_sinkage(ahead, s, t, load);
    if (z <= ahead.rut) return 0.0f;
    const float w = t.width + ahead.gravel;
    const float e = s.n + 1.0f;
    return w * modulus(s, w) * (std::pow(z, e) - std::pow(ahead.rut, e)) / e;
}

Contact contact(const Cell& c, const SoilParams& s, const Tyre& t, float load) {
    if (c.paving != Paving::None) {  // asphalt, perhaps with loose gravel lying on it
        const Contact road = hard_contact(c.paving == Paving::Rolled ? kAsphaltRolled : kAsphaltLaid, t, load, false);
        const float cover = std::clamp(c.loose / kGravelFullCover, 0.0f, 1.0f);
        if (cover <= 0.0f) return road;
        const Contact top = hard_contact(kGravel, t, load, true);
        return {std::lerp(road.max_traction, top.max_traction, cover), std::lerp(road.slip_gain, top.slip_gain, cover),
                std::lerp(road.rolling, top.rolling, cover), 0.0f, true};
    }

    Contact base;
    if (c.soft <= 0.0f) {
        base = hard_contact(kFirmDirt, t, load, true);
    } else if (c.soft - c.rut < 0.01f) {
        base = hard_contact(kBottomedOut, t, load, true);
        base.rolling += s.rut_drag * c.rut * load;
        base.softness = 1.0f;
    } else {
        const float area = t.width * t.length;
        base.max_traction = (area * s.cohesion + load * s.tan_phi) * t.soft_grip;
        base.slip_gain = t.length / s.shear_k;
        base.rolling = (0.04f + s.rut_drag * c.rut) * load;
        base.softness = 1.0f;
        base.diggable = true;
    }
    const float cover = std::clamp(c.gravel / kGravelFullCover, 0.0f, 1.0f);
    if (cover <= 0.0f) return base;
    const Contact top = hard_contact(kGravel, t, load, true);
    return {std::lerp(base.max_traction, top.max_traction, cover), std::lerp(base.slip_gain, top.slip_gain, cover),
            std::lerp(base.rolling, top.rolling, cover), std::lerp(base.softness, top.softness, cover), true};
}

}  // namespace inc
