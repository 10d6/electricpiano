// The electrical half: preamps, tremolo/vibrato, speakers, and what goes wrong
// with old electronics (hum, hiss, crackle, scratchy pots, tired preamps,
// rattling speakers).
#pragma once

#include <synthkit/synth.h>

#include <array>
#include <cmath>
#include <vector>

namespace ep {

// 2x oversampling with a 31-tap half-band FIR (windowed sinc, ~-80 dB stop
// band), for the preamp's distortion.
class HalfBand {
 public:
  HalfBand() {
    for (int i = 0; i < kTaps; ++i) {
      const int k = i - kCenter;
      double h = 0;
      if (k == 0) h = 0.5;
      else if (k % 2 != 0) {
        const double x = 3.141592653589793 * k / 2;
        const double win = 0.42 + 0.5 * std::cos(3.141592653589793 * k / (kCenter + 1)) +
                           0.08 * std::cos(2 * 3.141592653589793 * k / (kCenter + 1));
        h = std::sin(x) / (3.141592653589793 * k) * win;
      }
      h_[static_cast<std::size_t>(i)] = static_cast<float>(h);
    }
  }
  void reset() { up_.fill(0); dn_.fill(0); upPos_ = dnPos_ = 0; }

  // One input sample -> two output samples at 2x.
  void up(float x, float& a, float& b) {
    upPos_ = (upPos_ + 1) % kHist;
    up_[static_cast<std::size_t>(upPos_)] = x;
    // y[2n] = 2 * sum over even taps (odd offsets) ; y[2n+1] = x delayed
    float s = 0;
    for (int i = 0; i < kTaps; i += 2) s += h_[static_cast<std::size_t>(i)] * at(up_, upPos_, i / 2);
    a = 2 * s;
    b = at(up_, upPos_, kCenter / 2);
  }
  // Two samples at 2x -> one output sample.
  float down(float a, float b) {
    push(a);
    push(b);
    float s = 0;
    for (int i = 0; i < kTaps; ++i) {
      const float hi = h_[static_cast<std::size_t>(i)];
      if (hi != 0) s += hi * at(dn_, dnPos_, i);
    }
    return s;
  }

 private:
  static constexpr int kTaps = 31, kCenter = 15, kHist = 32;
  std::array<float, kTaps> h_{};
  std::array<float, kHist> up_{}, dn_{};
  int upPos_ = 0, dnPos_ = 0;
  void push(float x) { dnPos_ = (dnPos_ + 1) % kHist; dn_[static_cast<std::size_t>(dnPos_)] = x; }
  static float at(const std::array<float, kHist>& b, int pos, int ago) {
    return b[static_cast<std::size_t>((pos - ago + kHist) % kHist)];
  }
};

// A preamp/power-amp stage's clipper: asymmetric (bias: even harmonics), with
// a knee from soft (hard = 0: y / sqrt(1 + y^2)) to firm (hard = 1: a 4th-power knee).
inline float clip01(float y, float hard) {
  const float y2 = y * y;
  const float soft = y / std::sqrt(1 + y2);
  const float firm = y / std::sqrt(std::sqrt(1 + y2 * y2));
  return soft + (firm - soft) * hard;
}
inline float saturate(float x, float bias, float hard, float atRest) { return clip01(x + bias, hard) - atRest; }


// A small room: an 8-line feedback delay network (Householder mixing), darker
// as it decays. Cheap enough to leave running.
class Room {
 public:
  void prepare(double sr) {
    static const int base[kN] = {557, 683, 811, 937, 1061, 1193, 1319, 1451};   // at 48 kHz
    for (int i = 0; i < kN; ++i) {
      len_[i] = std::max(8, static_cast<int>(base[i] * sr / 48000.0));
      buf_[i].assign(static_cast<std::size_t>(len_[i]), 0.f);
      pos_[i] = 0;
      lp_[i] = 0;
      // T60 of about 0.55 s
      g_[i] = static_cast<float>(std::pow(10.0, -3.0 * len_[i] / (0.55 * sr)));
    }
    damp_ = static_cast<float>(1 - std::exp(-2 * 3.141592653589793 * 4500 / sr));
  }
  void process(float& l, float& r) {
    float o[kN], d[kN];
    float sum = 0;
    for (int i = 0; i < kN; ++i) {
      o[i] = buf_[i][static_cast<std::size_t>(pos_[i])];
      lp_[i] += damp_ * (o[i] - lp_[i]);
      d[i] = lp_[i];
      sum += d[i];
    }
    const float mix = sum * (2.0f / kN);
    const float in = (l + r) * 0.25f;
    float outL = 0, outR = 0;
    for (int i = 0; i < kN; ++i) {
      buf_[i][static_cast<std::size_t>(pos_[i])] = (d[i] - mix) * g_[i] + (i & 2 ? in : -in);
      if (++pos_[i] >= len_[i]) pos_[i] = 0;
      (i & 1 ? outR : outL) += o[i];
    }
    l = outL * 0.5f;
    r = outR * 0.5f;
  }

 private:
  static constexpr int kN = 8;
  std::vector<float> buf_[kN];
  int len_[kN]{}, pos_[kN]{};
  float lp_[kN]{}, g_[kN]{};
  float damp_ = 0.5f;
};

enum class Amp { Stage = 0, Suitcase = 1, Wurli = 2, Direct = 3 };

struct AmpVoice {
  float bias, hard;          // clipper
  float pickupLp, pickupQ;   // pickup/cable resonance (Hz, Web Audio dB Q)
  float hp, hpQ;             // speaker low end
  float peak, peakDb, peakQ; // speaker presence
  float lp;                  // speaker top end
  float level;               // makeup
  bool stereoTrem;           // Suitcase: the vibrato pans between two speaker pairs
};

inline const AmpVoice& ampVoice(Amp a) {
  // Stage piano into a Fender Twin; Suitcase (Peterson preamp, 4x12");
  // Wurlitzer 200A (its own amp, two small ovals); Direct (line out).
  static const AmpVoice v[4] = {
      {0.18f, 0.0f, 5200, 4.0f, 70, 2.0f, 2400, 2.5f, 1.0f, 6500, 1.0f, false},
      {0.05f, 0.8f, 5200, 4.0f, 85, 3.0f, 1300, 2.0f, 0.8f, 5200, 1.0f, true},
      {0.30f, 0.5f, 9000, 0.0f, 190, 5.0f, 1800, 5.0f, 1.1f, 4600, 1.0f, false},
      {0.02f, 0.0f, 7000, 1.0f, 25, 0.0f, 1000, 0.0f, 1.0f, 18000, 1.0f, false},
  };
  return v[static_cast<int>(a)];
}

}  // namespace ep
