#include "inc/soil.h"

#include <algorithm>
#include <cmath>

namespace inc {
namespace {

float modulus(const SoilParams& s, float width) { return s.k_c / width + s.k_phi; }

Contact hard_contact(float mu, float crr, const SoilParams& s, const Tyre& t, float load, bool diggable) {
    return {mu * load, s.hard_slip_gain, crr * t.crr_scale * load, 0.0f, diggable};
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
        const bool rolled = c.paving == Paving::Rolled;
        const Contact road = hard_contact(t.grip.asphalt * (rolled ? 1.0f : s.laid_grip), rolled ? s.crr_rolled : s.crr_laid, s, t, load, false);
        const float cover = std::clamp(c.loose / s.gravel_full_cover, 0.0f, 1.0f);
        if (cover <= 0.0f) return road;
        const Contact top = hard_contact(t.grip.gravel, s.crr_gravel, s, t, load, true);
        return {std::lerp(road.max_traction, top.max_traction, cover), std::lerp(road.slip_gain, top.slip_gain, cover),
                std::lerp(road.rolling, top.rolling, cover), 0.0f, true};
    }

    Contact base;
    if (c.soft <= 0.0f) {
        base = hard_contact(t.grip.firm, s.crr_firm, s, t, load, true);
    } else if (c.soft - c.rut < 0.01f) {
        base = hard_contact(t.grip.wet, s.crr_firm, s, t, load, true);
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
    const float cover = std::clamp(c.gravel / s.gravel_full_cover, 0.0f, 1.0f);
    if (cover <= 0.0f) return base;
    const Contact top = hard_contact(t.grip.gravel, s.crr_gravel, s, t, load, true);
    return {std::lerp(base.max_traction, top.max_traction, cover), std::lerp(base.slip_gain, top.slip_gain, cover),
            std::lerp(base.rolling, top.rolling, cover), std::lerp(base.softness, top.softness, cover), true};
}

}  // namespace inc
