#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Synthesised sound, no audio files. Independent of SDL so it can also render to a file.
struct SoundState {
    float rpm = 0.0f;
    float throttle = 0.0f;
    float slip = 0.0f;  // fastest wheel slip [m/s]
    bool running = false;
    bool grinding = false;
    bool starter = false;
};

class Synth {
public:
    explicit Synth(float sample_rate) : rate_(sample_rate) {}

    void render(float* out, int n, const SoundState& s) {
        constexpr float kTau = 6.2831853f;
        const float smooth = pole(4.0f), wobble = pole(30.0f), bright = pole(2500.0f), dull = pole(300.0f);
        // More throttle: louder and brighter (upper harmonics fall off more slowly).
        float amp[6];
        for (int h = 1; h <= 5; ++h) amp[h] = 1.0f / std::pow(static_cast<float>(h), 1.3f - 0.6f * load_);

        for (int i = 0; i < n; ++i) {
            rpm_ += (s.rpm - rpm_) * smooth;
            load_ += (s.throttle - load_) * smooth;
            slip_ += (std::min(s.slip / 5.0f, 1.0f) - slip_) * smooth;
            fire_ += ((s.running ? 1.0f : 0.0f) - fire_) * smooth;

            const float white = noise();
            rough_ += (white - rough_) * wobble;
            hiss_ += (white - hiss_) * bright;
            body_ += (hiss_ - body_) * dull;
            const float band = hiss_ - body_;  // roughly 300 Hz to 2.5 kHz

            // Six-cylinder four-stroke: one cycle is two crank turns, with six firings in it.
            cycle_ += rpm_ / 120.0f / rate_;
            cycle_ -= std::floor(cycle_);
            float engine = 0.0f;
            for (int h = 1; h <= 5; ++h) engine += amp[h] * std::sin(kTau * 6.0f * h * cycle_ + 0.7f * h);
            engine += 0.30f * std::sin(kTau * 3.0f * cycle_) + 0.20f * std::sin(kTau * 9.0f * cycle_ + 1.0f) +
                      0.15f * std::sin(kTau * cycle_);  // half orders: the uneven diesel rumble
            engine = engine * (1.0f + 3.0f * rough_) + 2.0f * load_ * band;

            const float firing = (0.25f + 0.55f * load_) * (0.6f + 0.4f * rpm_ / 2500.0f);
            const float cranking = 0.06f * std::min(rpm_ / 600.0f, 1.0f);
            float mix = 0.30f * engine * (fire_ * firing + (1.0f - fire_) * cranking);
            mix += 2.0f * slip_ * band;  // wheels spinning
            if (s.grinding) {
                grind_ += 160.0f / rate_;
                grind_ -= std::floor(grind_);
                mix += 0.30f * (2.0f * grind_ - 1.0f) * (0.5f + 3.0f * std::abs(band));
            }
            if (s.starter && !s.running) {
                starter_ += 1.0f / rate_;
                const float saw = std::fmod(starter_ * 110.0f, 1.0f) * 2.0f - 1.0f;
                mix += 0.20f * saw * (0.5f + 0.5f * std::sin(kTau * 9.0f * starter_));
            }
            out[i] = std::tanh(mix);
        }
    }

private:
    float pole(float hz) const { return 1.0f - std::exp(-6.2831853f * hz / rate_); }
    float noise() {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<float>(seed_) / 2147483648.0f - 1.0f;
    }

    float rate_;
    float rpm_ = 0.0f, load_ = 0.0f, slip_ = 0.0f, fire_ = 0.0f;
    float cycle_ = 0.0f, grind_ = 0.0f, starter_ = 0.0f;
    float rough_ = 0.0f, hiss_ = 0.0f, body_ = 0.0f;
    std::uint32_t seed_ = 0x2545F491u;
};
