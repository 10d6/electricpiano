// The mechanical half of an electric piano: a hammer that hits a vibrating
// part (a Rhodes tine with its tonebar, or a Wurlitzer reed), dampers, and the
// pickup that turns the motion into voltage.
//
// Everything here is plain physics with a few tables of measurements-by-ear:
//   - the hammer is a mass on a nonlinear felt/neoprene spring (Hunt-Crossley
//     contact), so harder hits are shorter and brighter by themselves;
//   - the vibrating part is a set of modes (two-pole resonators), driven at the
//     strike point and read at the tip, with the hammer feeling the part recoil;
//   - a Rhodes tine is coupled to its tonebar: the pair's two normal modes are
//     worked out per note, so a well-tuned tonebar shares energy and sustains,
//     and a detuned one beats;
//   - the pickup is a nonlinear function of the tip's position, and its output
//     is the rate of change of that function (magnetic flux, or capacitance).
#pragma once

#include <synthkit/synth.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ep {

using sk::dsp::Rng;

constexpr double kTwoPi = 6.283185307179586;

enum class Mech { Tine = 0, Reed = 1 };
enum class Pickup { Magnetic = 0, Electrostatic = 1 };

// One mode of vibration: q'' + 2/tau q' + w^2 q = phi * F / M, as a rotating
// phasor z (q = Im z). Retuning (pitch bend) turns the rotation without
// touching |z|, so the mode keeps its energy; the decay is exact.
struct Mode {
  double zr = 0, zi = 0;     // phasor; zi is the modal displacement
  double c = 1, s = 0, r = 0;
  double gin = 0;            // input gain: force (per sample) -> zr
  double phi = 0;            // mode shape at the strike point (hammer sees phi * q)
  double tip = 1;            // mode shape at the pickup (tip)
  bool on = false;

  void reset() { zr = zi = 0; }
  double q() const { return zi; }
  double amplitude() const { return std::sqrt(zr * zr + zi * zi); }

  // hz: frequency, M: effective mass, sr: sample rate
  void tune(double hz, double phiStrike, double tipShape, double M, double sr) {
    const double w = kTwoPi * hz / sr;
    on = hz > 10 && hz < 0.46 * sr;
    c = std::cos(w);
    s = std::sin(w);
    phi = phiStrike;
    tip = tipShape;
    // An impulse J gives q = phi J / (M wc) sin(wc t).
    gin = on ? phiStrike / (M * kTwoPi * hz) / sr : 0;
  }
  void setDecay(double rate, double sr) { r = std::exp(-rate / sr); }
  double step(double force) {
    const double a = zr + gin * force;
    const double nr = r * (c * a - s * zi);
    zi = r * (s * a + c * zi);
    zr = nr;
    return zi;
  }
};

// T60 (seconds) <-> decay rate (1/s)
inline double rateFromT60(double t60) { return 6.907755 / std::max(1e-3, t60); }

// The hammer: a unit mass flying at the part, with a felt/neoprene tip whose
// force grows as compression^p, plus hysteresis (felt doesn't give back all it
// takes).
struct Hammer {
  double x = 0, v = 0;       // position (toward the part is +) and speed
  double K = 0, p = 2.5, lam = 0;
  double prevDelta = 0;
  int sinceContact = 0;      // samples since the last contact ended
  int holdSamples = 0;       // keep simulating this long after a contact, for re-hits
  bool flying = false, touching = false;

  // contactT: contact time (s) at full speed (v = 1); the speed sets how hard
  void launch(double speed, double contactT, double power, double partPos, double sr) {
    p = power;
    // Max compression of a power-law spring at v = 1, m = 1 is
    // ((p+1)/(2K))^(1/(p+1)); the contact lasts about 3x that / v.
    const double dm = contactT / 3.0;
    K = (p + 1) / (2 * std::pow(dm, p + 1));
    lam = 0.55 / std::max(speed, 0.05);
    v = speed;
    x = partPos - speed * 0.5 / sr;
    prevDelta = 0;
    flying = true;
    touching = false;
    sinceContact = 0;
    holdSamples = static_cast<int>(sr * contactT * 4) + 8;
  }

  // partPos: where the part is at the strike point. Returns the force on the part.
  double step(double partPos, double sr) {
    if (!flying) return 0;
    const double dt = 1 / sr;
    const double delta = x - partPos;
    double F = 0;
    if (delta > 0) {
      const double ddelta = (delta - prevDelta) * sr;
      F = K * std::pow(delta, p) * (1 + lam * ddelta);
      if (F < 0) F = 0;
      touching = true;
      sinceContact = 0;
    } else {
      if (touching) touching = false;
      if (++sinceContact > holdSamples && v <= 0) flying = false;
    }
    prevDelta = std::max(0.0, delta);
    v -= F * dt;
    x += v * dt;
    return F;
  }
};

// Pickups. u is the tip's position in units of the pickup's reach; h is how far
// the tip's rest position sits from the pickup's centre (the voicing).
//
// Magnetic (Rhodes): the flux through the coil peaks when the tine tip is in
// line with the pole piece and falls off either side. A centred tine swings
// symmetrically over the peak (strong 2nd harmonic: bell); an offset one rides
// the slope (more fundamental, and growl when it swings over the top).
inline double fluxMagnetic(double u) { return 1.0 / (1.0 + u * u); }
// Electrostatic (Wurlitzer): the reed moves in and out of a slot in a charged
// plate, so capacitance is a smoothed step: it rises as the reed enters and
// saturates inside. Swinging across both edges squares the wave (reedy, odd
// harmonics); an offset rest position adds even ones.
inline double capElectrostatic(double u) { return 0.5 * std::tanh(2.8 * u); }

// How an instrument's design varies across the keyboard.
struct Design {
  Mech mech;
  double M;                  // effective mass of the part, relative to the hammer
  double contactT;           // contact time (s) at full speed, at 110 Hz
  double contactTilt;        // contact time ~ (f/110)^tilt
  double power;              // felt spring exponent
  double t60;                // fundamental T60 at 110 Hz
  double t60Tilt;
  double t60Lo, t60Hi;
  double damp60;             // damped T60 at 110 Hz
  double ratio2, ratio3;     // overtone frequency ratios
  double w2, w3;             // overtone excitation, relative
  double t2, t3;             // overtone T60 at 110 Hz
  double phi1;               // fundamental's mode shape at the strike point
  double jRef;               // momentum a full-speed strike passes to the part (the pickup's yardstick)
  double levelTilt;          // output level ~ (f/262)^tilt
  double gain;
};

inline const Design& design(Mech m) {
  static const Design tine{Mech::Tine, 0.6, 1.7e-3, -0.5, 2.3,
                           8.0, -0.55, 1.2, 22.0, 0.16,
                           6.267, 17.55, 0.16, 0.035, 0.9, 0.22,
                           0.3, 1.45, 0.15, 1.0};
  static const Design reed{Mech::Reed, 0.3, 0.9e-3, -0.3, 2.8,
                           6.0, -0.65, 0.7, 13.0, 0.12,
                           7.1, 19.6, 0.015, 0.004, 0.45, 0.12,
                           0.6, 0.48, 0.1, 1.0};
  return m == Mech::Tine ? tine : reed;
}

inline double tilt(double hz, double ref, double t) { return std::pow(hz / ref, t); }

// What age has done to one key. Drawn from the instrument's serial number, so
// the same instrument always has the same faults; each fault's dice are thrown
// whatever its amount, so turning a wear knob scales the same faults smoothly.
struct KeyTraits {
  double cents = 0;          // tuning error
  double tonebar = 0;        // tonebar (or reed tip) detune, as a ratio offset
  double offset = 0;         // pickup alignment error (added to h)
  double gap = 1;            // pickup distance factor
  double level = 1;          // output level factor
  double hard = 1;           // contact-time factor (< 1 = harder hammer tip)
  double power = 0;          // added to the felt exponent (hardened tips are more linear)
  double velScale = 1;       // velocity sensitivity of this key
  double bounce = 0;         // chance per strike of a second, weaker hit
  double damperWeak = 0;     // 0 = good damper, 1 = barely touches
  double damperDelay = 0;    // seconds before the damper lands
  bool damperOff = false;    // damper doesn't seat: rings and buzzes
  double rattleThr = 99;     // tip excursion above which something rattles
  double rattleGain = 0;
  bool dead = false;
  // for the page's wear map: 0...1 summaries
  double mapVoicing = 0, mapMech = 0;
};

struct Wear {
  double tuning = 0, voicing = 0, hammers = 0, dampers = 0, action = 0, rattle = 0;
};

inline KeyTraits keyTraits(int note, int serial, const Wear& w, bool serviced) {
  Rng rng(sk::dsp::hashSeed(static_cast<uint32_t>(serial), static_cast<uint32_t>(note), 0xE1EC7u));
  // Every draw, always, in this order.
  double n[8];
  for (double& x : n) x = rng.normal();
  double u[12];
  for (double& x : u) x = rng.uniform();
  KeyTraits k;
  // Even a well-kept instrument's tonebars sit a little off their tines.
  k.tonebar = n[7] * 0.0018;
  if (serviced) return k;

  // Tuning: a spread, plus the odd key that lost its tuning spring's grip or a
  // blob of reed solder (way out).
  k.cents = n[0] * 9.0 * w.tuning;
  if (u[0] < 0.07 * w.tuning) k.cents += (u[1] < 0.5 ? -1 : 1) * (22 + 40 * u[2]) * w.tuning;
  k.tonebar += n[1] * 0.004 * w.tuning;

  // Voicing: pickups knocked out of line, different distances, uneven level.
  k.offset = n[2] * 0.35 * w.voicing;
  k.gap = std::exp(n[3] * 0.3 * w.voicing);
  k.level = std::exp(n[4] * 0.22 * w.voicing);
  k.mapVoicing = std::min(1.0, (std::abs(n[2]) * 0.35 + std::abs(n[3]) * 0.3 + std::abs(n[4]) * 0.2) * w.voicing);

  // Hammers: tips harden and groove: shorter, clackier, less velocity-sensitive.
  k.hard = std::exp(n[5] * 0.25 * w.hammers) * (1 - 0.45 * w.hammers);
  k.power = -0.7 * w.hammers * (0.6 + 0.4 * u[3]);

  // Action: uneven keys, bounces, the odd near-dead key.
  k.velScale = std::exp(n[6] * 0.25 * w.action);
  k.bounce = 0.35 * w.action * u[4];
  k.dead = u[5] < 0.035 * w.action;
  if (k.dead) k.level *= 0.15 + 0.25 * u[6];

  // Dampers: felt wears thin, lands late, or stops seating.
  k.damperWeak = std::pow(u[7], 1.5) * w.dampers;
  k.damperDelay = 0.004 + 0.07 * w.dampers * u[8] * u[8];
  k.damperOff = u[9] < 0.06 * w.dampers;

  // Rattles: a loose tonebar screw or reed clamp, on some keys.
  if (u[10] < 0.35 * w.rattle) {
    k.rattleThr = 1.4 - 0.9 * w.rattle * u[11];
    k.rattleGain = 0.4 * w.rattle;
  }
  k.mapMech = std::min(1.0, std::max({k.damperWeak, k.damperOff ? 1.0 : 0.0, k.dead ? 1.0 : 0.0,
                                      k.rattleGain > 0 ? 0.5 + 0.5 * w.rattle : 0.0,
                                      std::abs(n[5]) * 0.25 * w.hammers, k.bounce * 2}));
  return k;
}

}  // namespace ep
