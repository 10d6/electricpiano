// Electric Piano: physically modelled Rhodes-style tines and Wurlitzer-style
// reeds, their pickups and electronics, and the faults old instruments pick up.
//
//   Physics.h      hammer, modes, tine/tonebar coupling, pickups, per-key wear
//   Electronics.h  oversampling, preamp clipping, amp/speaker voicings
//   this file      parameters, the voice (one per key, re-struck in place),
//                  the global chain (preamp, tremolo, cabinet, room, noises)
#include <synthkit/synth.h>

#include <array>
#include <cmath>
#include <vector>

#include "Electronics.h"
#include "Physics.h"

using namespace sk;
using ep::Mech;
using ep::Pickup;

struct ElectricPiano;

struct ElectricPianoVoice : Voice {
  explicit ElectricPianoVoice(ElectricPiano& s) : synth(s) {}
  void prepare(const PrepareInfo& info) override;
  void start(const NoteOn& n, const StartInfo& s) override;
  void release() override;
  void render(VoiceContext& ctx, Buffer& out) override;
  bool active() const override;

  ElectricPiano& synth;
  double sr = 48000;

  // what this key is
  int note = 60;
  Mech mech = Mech::Tine;
  Pickup pickup = Pickup::Magnetic;
  ep::KeyTraits kt;
  double f1 = 261.6;         // fundamental at the strike (Hz)
  double aref = 1;           // tip amplitude of a full-speed strike, for the pickup's scale
  double outGain = 0;        // pickup output -> voice output
  double tonebar = 0, coupling = 0;

  // mechanics
  std::array<ep::Mode, 4> modes;
  ep::Hammer hammer;
  bool pendingStrike = false, firstTune = true;
  double strikeSpeed = 0;
  int bounceIn = -1;
  double bounceSpeed = 0;
  double lastHz = 0;

  // dampers
  bool released = false;
  double damperIn = -1;      // seconds until the damper lands (< 0: not falling)
  double damp = 0;           // 0 = lifted ... 1 = fully on
  double dampTarget = 0;
  double dampStrength = 1;
  int thunkPos = -1, thunkLen = 1;
  double thunkJ = 0;

  // pickup and output
  double phiPrev = 0;
  bool phiInit = false;
  double energy = 0, peak = 0;
  double age = 0;            // seconds since the strike

  // rattles and the action's own noise (mechanical, goes to the room not the pickup)
  dsp::Noise noise;
  dsp::Svf rattleBp;
  dsp::OnePole thudLp, clickHp, clickLp;
  double thud = 0, click = 0, clack = 0;
  double kThud = 0, kClick = 0, kClack = 0;
  int clackIn = -1;
};

struct ServiceTable {
  std::array<uint8_t, 128> keys{};  // 1 = this key has been serviced (faults removed)
};

struct ElectricPiano : Synth {
  // instrument
  Param mechType, pickupType, ampType;
  // hammer & action
  Param hardness, strike, velCurve, damperAmt, actionNoise;
  // pickup
  Param pickOffset, pickGap;
  // electronics
  Param drive, bass, treble, speaker, tremDepth, tremRate, tremSync, tremDiv, roomMix;
  // condition
  Param age, serial, wTuning, wVoicing, wHammers, wDampers, wAction, wRattle, wElec, mains;
  // twists
  Param stretch, sustainMul, shimmer, sustainer;

  Custom<ServiceTable> service;
  MessageId wearMapReq, wearKey;

  // Global values cached once per block in processEvents, for tune() and
  // voices' start(), which have no parameter access of their own.
  Mech curMech = Mech::Tine;
  Pickup curPickup = Pickup::Magnetic;
  int curSerial = 1;
  ep::Wear curWear;
  double curCurve = 1, curHard = 0.5, curStrike = 0.5, curStretch = 1, curShimmer = 0, curActionNoise = 0;
  int mapNext = -1;          // next key of a wear map being sent to the page

  // the acoustic bus: the action's thumps, mixed in after the electronics
  std::array<float, kControlBlock> acoustic{};

  static constexpr const char* kDivisions[] = {"1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

  void defineParams(ParamBuilder& b) override {
    mechType = b.add("mech.type").name("Mechanism").choices({"Tine", "Reed"}).group("Instrument");
    pickupType = b.add("pickup.type").name("Pickup").choices({"Magnetic", "Electrostatic"}).group("Instrument");
    ampType = b.add("amp.type").name("Amp").choices({"Stage", "Suitcase", "200A", "Direct"}).group("Instrument");

    hardness = b.add("hammer.hardness").name("Hammer Hardness").range(0, 1).def(0.5).group("Action").modulatable();
    strike = b.add("hammer.strike").name("Strike Point").range(0, 1).def(0.5).group("Action");
    velCurve = b.add("action.curve").name("Velocity Curve").range(0.4, 2.5).exp(2).def(1.0).group("Action");
    damperAmt = b.add("damper.amount").name("Dampers").range(0, 1).def(1).group("Action").cc(67);
    actionNoise = b.add("noise.action").name("Action Noise").range(0, 1).def(0.15).group("Action");

    pickOffset = b.add("pickup.offset").name("Alignment").range(-1, 1).def(0.42).group("Pickup").modulatable();
    pickGap = b.add("pickup.gap").name("Distance").range(0.35, 3).log().def(1).group("Pickup").modulatable();

    drive = b.add("amp.drive").name("Drive").range(0, 1).def(0.2).group("Electronics").modulatable().cc(70);
    bass = b.add("amp.bass").name("Bass").range(-12, 12).def(0).unit("dB").group("Electronics");
    treble = b.add("amp.treble").name("Treble").range(-12, 12).def(0).unit("dB").group("Electronics").cc(74, -12, 12);
    speaker = b.add("amp.speaker").name("Cabinet").range(0, 1).def(1).group("Electronics");
    tremDepth = b.add("trem.depth").name("Trem Depth").range(0, 1).def(0).group("Tremolo").modulatable();
    tremRate = b.add("trem.rate").name("Trem Rate").range(0.5, 12).log().def(4.5).unit("Hz").group("Tremolo").modulatable();
    tremSync = b.add("trem.sync").name("Trem Sync").toggle().group("Tremolo");
    tremDiv = b.add("trem.division").name("Trem Division")
                  .choices({kDivisions[0], kDivisions[1], kDivisions[2], kDivisions[3], kDivisions[4], kDivisions[5],
                            kDivisions[6], kDivisions[7]})
                  .def(3).group("Tremolo");
    roomMix = b.add("room.mix").name("Room").range(0, 1).def(0.15).group("Electronics");

    age = b.add("wear.age").name("Age").range(0, 1).def(0.1).group("Condition");
    serial = b.add("wear.serial").name("Serial").range(1, 9999).integer().def(1973).group("Condition");
    wTuning = b.add("wear.tuning").name("Tuning").range(0, 1).def(0.5).group("Condition");
    wVoicing = b.add("wear.voicing").name("Voicing").range(0, 1).def(0.5).group("Condition");
    wHammers = b.add("wear.hammers").name("Hammer Tips").range(0, 1).def(0.5).group("Condition");
    wDampers = b.add("wear.dampers").name("Dampers").range(0, 1).def(0.5).group("Condition");
    wAction = b.add("wear.action").name("Action").range(0, 1).def(0.5).group("Condition");
    wRattle = b.add("wear.rattle").name("Rattles").range(0, 1).def(0.5).group("Condition");
    wElec = b.add("wear.electronics").name("Electronics").range(0, 1).def(0.5).group("Condition");
    mains = b.add("wear.mains").name("Mains").choices({"60 Hz", "50 Hz"}).group("Condition");

    stretch = b.add("twist.stretch").name("Partial Ratio").range(0.5, 2).log().def(1).group("Twists");
    sustainMul = b.add("twist.sustain").name("Sustain").range(0.25, 8).log().def(1).group("Twists").modulatable();
    shimmer = b.add("twist.shimmer").name("Shimmer").range(0, 1).def(0).group("Twists");
    sustainer = b.add("twist.sustainer").name("Sustainer").range(0, 1).def(0).group("Twists").modulatable();
  }

  void defineCustom(CustomBuilder& b) override {
    service = b.add<ServiceTable>("service", {
        .save = [](const ServiceTable& t, Json& j) {
          std::vector<double> keys;
          for (int i = 0; i < 128; ++i) if (t.keys[static_cast<std::size_t>(i)]) keys.push_back(i);
          j["keys"] = keys;
        },
        .load = [](const Json& j, ServiceTable& t) {
          t.keys.fill(0);
          for (const Json& k : j["keys"].items()) {
            const int n = static_cast<int>(k.number());
            if (n >= 0 && n < 128) t.keys[static_cast<std::size_t>(n)] = 1;
          }
        },
    });
  }

  void defineMessages(MessageBuilder& b) override {
    wearMapReq = b.add("wear.map");   // page -> engine: send me the wear map
    wearKey = b.add("wear.key");      // engine -> page: [note, cents, voicing 0..1, mechanics 0..1]
  }

  std::unique_ptr<Voice> makeVoice() override { return std::make_unique<ElectricPianoVoice>(*this); }

  ep::KeyTraits traits(int note) const {
    return ep::keyTraits(note, curSerial, curWear, service.read().keys[static_cast<std::size_t>(note & 127)] != 0);
  }

  // A struck key re-strikes its own tine/reed if it's still ringing, as on the
  // real instrument; otherwise the kit's allocation.
  int allocateVoice(const NoteOn& n, VoicePool& pool) override {
    for (int i = 0; i < pool.size(); ++i) {
      const auto st = pool.state(i);
      if (pool.note(i) == n.note && (st == VoicePool::State::Held || st == VoicePool::State::Released)) return i;
    }
    return pool.defaultAllocate(n);
  }

  double tune(int note, const NoteContext&) override { return note + traits(note).cents / 100.0; }

  // ---- global state ----
  double sr = 48000;
  wa::BiquadFilter pkLp, pkHp, eqLo, eqHi;
  wa::BiquadFilter cabHp[2], cabPk[2], cabLp[2];
  wa::BiquadFilter rattleBp;
  ep::Room room;
  ep::HalfBand osUp, osDown;
  dsp::Rng grng{0xA6E5};
  dsp::Noise hiss;
  double tremPhase = 0, humPhase = 0;
  double wobble = 0;
  double activity = 0;
  double crackleEnv = 0, crackleAmp = 0;
  double potGain = 1, potTarget = 1;
  int potHold = 0;
  float crackHpState = 0, crackPrev = 0;
  double kCrackle = 0;

  void prepare(const PrepareInfo& info) override {
    sr = info.sampleRate;
    kCrackle = std::exp(-1 / (0.0007 * sr));
    hiss.seed(0x5155);
    for (auto* f : {&pkLp, &pkHp, &eqLo, &eqHi, &rattleBp}) f->prepare(sr);
    for (int c = 0; c < 2; ++c)
      for (auto* f : {&cabHp[c], &cabPk[c], &cabLp[c]}) f->prepare(sr);
    pkLp.setType(wa::BiquadFilter::Type::Lowpass);
    pkHp.setType(wa::BiquadFilter::Type::Highpass);
    eqLo.setType(wa::BiquadFilter::Type::Lowshelf);
    eqHi.setType(wa::BiquadFilter::Type::Highshelf);
    rattleBp.setType(wa::BiquadFilter::Type::Bandpass);
    for (int c = 0; c < 2; ++c) {
      cabHp[c].setType(wa::BiquadFilter::Type::Highpass);
      cabPk[c].setType(wa::BiquadFilter::Type::Peaking);
      cabLp[c].setType(wa::BiquadFilter::Type::Lowpass);
    }
    osUp.reset();
    osDown.reset();
    room.prepare(sr);
  }

  void processEvents(EventContext& ctx) override {
    curMech = static_cast<Mech>(ctx.choice(mechType));
    curPickup = static_cast<Pickup>(ctx.choice(pickupType));
    curSerial = static_cast<int>(std::lround(ctx.get(serial)));
    const double a = ctx.get(age);
    auto eff = [&](Param p) { return std::clamp(2.0 * a * ctx.get(p), 0.0, 1.0); };
    curWear = {eff(wTuning), eff(wVoicing), eff(wHammers), eff(wDampers), eff(wAction), eff(wRattle)};
    curCurve = ctx.get(velCurve);
    curHard = ctx.get(hardness);
    curStrike = ctx.get(strike);
    curStretch = ctx.get(stretch);
    curShimmer = ctx.get(shimmer);
    curActionNoise = ctx.get(actionNoise);

    for (auto& m : ctx.messages())
      if (m.is(wearMapReq)) mapNext = 21;
    // The map goes out a few keys per block, so the page's queue never fills.
    for (int sent = 0; mapNext >= 21 && mapNext <= 108 && sent < 24; ++sent, ++mapNext) {
      const auto k = traits(mapNext);
      if (!ctx.post(wearKey, {static_cast<float>(mapNext), static_cast<float>(k.cents),
                              static_cast<float>(k.mapVoicing), static_cast<float>(k.mapMech)}))
        break;
    }
    if (mapNext > 108) mapNext = -1;

    for (auto& e : ctx.in()) ctx.out(e);
  }

  void processGlobal(GlobalContext& ctx, Buffer& io) override;
};

// ---------------------------------------------------------------------------
// Voice

void ElectricPianoVoice::prepare(const PrepareInfo& info) {
  sr = info.sampleRate;
  rattleBp.prepare(static_cast<float>(sr));
  rattleBp.setMode(dsp::Svf::Mode::BandPass);
  thudLp.prepare(static_cast<float>(sr));
  clickHp.prepare(static_cast<float>(sr));
  thudLp.set(260);
  clickLp.prepare(static_cast<float>(sr));
  clickLp.set(4500);
  kThud = std::exp(-1 / (0.018 * sr));
  kClick = std::exp(-1 / (0.0015 * sr));
  kClack = std::exp(-1 / (0.004 * sr));
  clickHp.set(1200);
}

void ElectricPianoVoice::start(const NoteOn& n, const StartInfo& s) {
  const bool restrike = s.sounding && n.note == note;
  note = n.note;
  kt = synth.traits(note);
  if (!restrike) {
    // A fresh key: everything at rest.
    for (auto& m : modes) m.reset();
    phiInit = false;
    damp = 0;
    energy = 0;
    firstTune = true;
    mech = synth.curMech;
    pickup = synth.curPickup;
    rattleBp.reset();
  }
  // Re-struck while ringing: the damper lifts and the hammer meets the part
  // wherever it is.
  released = false;
  damperIn = -1;
  dampTarget = 0;
  thunkPos = -1;
  clackIn = -1;
  age = 0;

  // hammer speed: about 15 dB from pp to ff before the pickup's own expansion
  double v = std::pow(std::max(0.0f, n.vel), static_cast<float>(1.5 * synth.curCurve)) * kt.velScale;
  v = std::clamp(v, 0.02, 1.25);
  strikeSpeed = v;
  pendingStrike = true;

  bounceIn = -1;
  if (rng().chance(static_cast<float>(kt.bounce))) {
    bounceIn = static_cast<int>((0.012 + 0.03 * rng().uniform()) * sr);
    bounceSpeed = v * (0.1 + 0.22 * rng().uniform());
  }
  noise.seed(rng().integer(1, 1 << 30));

  // the key hitting the keybed
  const double an = synth.curActionNoise * (1 + 2 * synth.curWear.action) * 0.05;
  thud = an * (0.4 + v);
  click = an * 0.35 * v * v * (1 + 1.5 * synth.curWear.hammers);
}

void ElectricPianoVoice::release() {
  released = true;
  damperIn = kt.damperDelay;
  clackIn = static_cast<int>(0.012 * sr);
}

bool ElectricPianoVoice::active() const {
  if (pendingStrike || hammer.flying || thunkPos >= 0 || thud > 1e-5 || clack > 1e-5 || clackIn >= 0) return true;
  if (released) return energy > 2e-4;
  return energy > 2e-5;
}

void ElectricPianoVoice::render(VoiceContext& ctx, Buffer& out) {
  const ep::Design& d = ep::design(mech);
  const double hz = ctx.pitchHz();

  // ---- (re)tune the modes: once per strike for levels, per block for pitch ----
  if (firstTune || pendingStrike || std::abs(hz - lastHz) > 1e-6 * hz) {
    if (firstTune) {
      f1 = hz;
      firstTune = false;
      tonebar = kt.tonebar + 0.006 * synth.curShimmer;
      coupling = (mech == Mech::Tine ? 0.0006 : 0.0) + 0.012 * synth.curShimmer * synth.curShimmer;
    }
    lastHz = hz;
    const double stretchK = synth.curStretch;
    const double strikeK = std::pow(4.0, (synth.curStrike - 0.5) * 2);   // 1/4 ... 4
    // tine (or reed) and tonebar: normal modes of two coupled oscillators
    const double a = hz * hz, b = hz * hz * (1 + tonebar) * (1 + tonebar), k = coupling * hz * hz;
    const double mean = 0.5 * (a + b), diff = 0.5 * (a - b), disc = std::sqrt(diff * diff + k * k);
    const double th = 0.5 * std::atan2(2 * k, a - b);
    const double cA = std::cos(th), cB = -std::sin(th);
    modes[0].tune(std::sqrt(mean + disc), d.phi1 * cA, cA, d.M, sr);
    modes[1].tune(std::sqrt(std::max(1.0, mean - disc)), d.phi1 * cB, cB, d.M, sr);
    if (std::abs(cB) < 1e-4) modes[1].on = false;
    // overtones: near the clamp, the mode shape grows as the ratio, so a
    // strike there excites them all about equally; the contact time filters
    const double r2 = std::max(1.5, d.ratio2 * stretchK), r3 = std::max(2.0, d.ratio3 * stretchK);
    modes[2].tune(hz * r2, d.phi1 * d.ratio2 * d.w2 * strikeK, 1, d.M, sr);
    modes[3].tune(hz * r3, d.phi1 * d.ratio3 * d.w3 * strikeK * strikeK, 1, d.M, sr);
  }

  // ---- decay rates: free, or with the damper on ----
  {
    const double sus = ctx.get(synth.sustainMul);
    const double t60 = std::clamp(d.t60 * ep::tilt(f1, 110, d.t60Tilt), d.t60Lo, d.t60Hi) * sus;
    const double rTine = ep::rateFromT60(t60);
    const double rBar = ep::rateFromT60(t60 * 2.2);     // the tonebar rings longer
    const double r2 = ep::rateFromT60(d.t2 * ep::tilt(f1, 110, -0.5) * std::sqrt(sus));
    const double r3 = ep::rateFromT60(d.t3 * ep::tilt(f1, 110, -0.5) * std::sqrt(sus));
    double damped = ep::rateFromT60(std::clamp(d.damp60 * ep::tilt(f1, 110, -0.45), 0.03, 0.8) *
                                    (1 + 18 * kt.damperWeak * kt.damperWeak));
    if (kt.damperOff) damped = rTine * 3;
    const double c0 = modes[0].tip * modes[0].tip, c1 = modes[1].tip * modes[1].tip;
    const double free[4] = {c0 * rTine + c1 * rBar, c1 * rTine + c0 * rBar, r2, r3};
    // the damper's felt also takes the overtones
    const double dampR[4] = {damped, damped * 0.8, damped * 2, damped * 3};

    // the damper falling, landing and ramping in
    const double blockT = out.frames / sr;
    if (released && damperIn >= 0) {
      damperIn -= blockT;
      if (damperIn < 0) {
        dampTarget = 1;
        dampStrength = ctx.get(synth.damperAmt) * (kt.damperOff ? 0.25 : 1.0);
        thunkPos = 0;   // the felt lands with a small thump of its own
        thunkLen = std::max(4, static_cast<int>(0.003 * sr));
        thunkJ = -0.05 * dampStrength;
      }
    }
    const double k = 1 - std::exp(-blockT / (dampTarget > damp ? 0.012 : 0.004));
    damp += (dampTarget - damp) * k;
    const double dd = damp * dampStrength;
    for (int i = 0; i < 4; ++i) {
      // blend decay rates in the log domain (a damper squeezing in)
      double rate = free[i] + (std::max(free[i], dampR[i]) - free[i]) * dd;
      if (released) rate = std::max(rate, ep::rateFromT60(10));   // nothing rings forever once let go
      modes[static_cast<std::size_t>(i)].setDecay(rate, sr);
    }
  }

  // ---- the hammer ----
  const double hardK = std::pow(2.0, (0.5 - ctx.get(synth.hardness)) * 3);   // x2.8 ... x0.35
  auto launch = [&](double speed) {
    double contactT = d.contactT * ep::tilt(f1, 110, d.contactTilt) * hardK * kt.hard;
    contactT = std::max(contactT, 10 / sr);
    double xs = 0;
    for (auto& m : modes) xs += m.phi * m.zi;
    hammer.launch(speed, contactT, std::max(1.2, d.power + kt.power), xs, sr);
  };
  if (pendingStrike) {
    pendingStrike = false;
    // tip amplitude of a full-speed strike, the pickup's yardstick
    aref = d.phi1 * d.jRef / (d.M * ep::kTwoPi * f1);
    launch(strikeSpeed);
    const double regLevel = ep::tilt(f1, 262, d.levelTilt);
    const double pickLevel = pickup == Pickup::Magnetic ? 1.0 : 0.5;   // the plate's swing is the larger
    outGain = sr / (ep::kTwoPi * f1) * regLevel * d.gain * pickLevel * kt.level * 0.26;
  }

  // ---- per-block pickup and twist settings ----
  const double gap = ctx.get(synth.pickGap) * kt.gap;
  // the reed's slot sits a little closer to centre than a tine's pole piece
  const double h = ctx.get(synth.pickOffset) + kt.offset - (pickup == Pickup::Electrostatic ? 0.12 : 0.0);
  const double uScale = 0.75 / (aref * gap);
  const double gapLevel = std::pow(gap, 0.55);           // closer = louder, but mostly brighter
  const double sustK = released ? 0.0 : 2 * (ctx.get(synth.sustainer) * 7.0) / sr * (1 - damp);   // sustainer loop gain
  // the sustainer's push fades out as the tine nears its target swing, so it
  // settles there (a quarter to 0.6 of a full-strength strike) instead of running away
  const double sustTarget = 0.25 + 0.35 * ctx.get(synth.sustainer);
  const double sustG = sustK * std::max(0.0, 1 - energy / sustTarget);
  const double rattleThr = released && kt.damperOff ? std::min(kt.rattleThr, 0.12) : kt.rattleThr;
  const double rattleGain = released && kt.damperOff ? std::max(kt.rattleGain, 0.25) : kt.rattleGain;
  if (rattleGain > 0) rattleBp.set(static_cast<float>(std::min(0.4 * sr, 2800.0 + 3 * f1)), 0.35f);
  const bool mag = pickup == Pickup::Magnetic;

  double blockPeak = 0;
  for (int i = 0; i < out.frames; ++i) {
    // strike-point position, for the hammer and the damper
    double xs = 0;
    for (auto& m : modes) xs += m.phi * m.zi;
    double F = hammer.step(xs, sr);
    if (bounceIn >= 0 && --bounceIn < 0) launch(bounceSpeed);
    if (thunkPos >= 0) {
      F += thunkJ * (3.141592653589793 / (2.0 * thunkLen / sr)) * std::sin(3.141592653589793 * thunkPos / thunkLen);
      if (++thunkPos >= thunkLen) thunkPos = -1;
    }

    double xt = 0;
    for (std::size_t m = 0; m < modes.size(); ++m) {
      auto& md = modes[m];
      if (!md.on) continue;
      if (m < 2) md.zr += sustG * md.zr;   // sustainer: push in step with the motion
      xt += md.tip * md.step(F);
    }

    // pickup: the rate of change of flux (or capacitance) as the tip moves
    const double u = xt * uScale - h;
    const double phi = mag ? ep::fluxMagnetic(u) : ep::capElectrostatic(u);
    if (!phiInit) { phiPrev = phi; phiInit = true; }
    double y = (phi - phiPrev) * outGain * gapLevel;
    phiPrev = phi;

    // something loose buzzing on big swings
    if (rattleGain > 0) {
      const double ex = std::abs(xt / aref) - rattleThr;
      if (ex > 0) y += rattleBp.process(noise.white()) * std::min(ex, 1.0) * rattleGain * 0.3;
    }
    out.add(i, static_cast<float>(y));
    blockPeak = std::max(blockPeak, std::abs(y));

    // the action, heard acoustically
    double ac = 0;
    if (thud > 1e-6 || click > 1e-6) {
      const double w = noise.white();
      ac += thudLp.lowpass(static_cast<float>(w)) * thud * 4 + clickLp.lowpass(clickHp.highpass(static_cast<float>(w))) * click;
      thud *= kThud;
      click *= kClick;
    }
    if (clackIn >= 0 && --clackIn < 0)
      clack = synth.curActionNoise * (1 + 2 * synth.curWear.action) * 0.005;
    if (clack > 1e-6) {
      ac += clickLp.lowpass(clickHp.highpass(static_cast<float>(noise.white()))) * clack;
      clack *= kClack;
    }
    synth.acoustic[static_cast<std::size_t>(i)] += static_cast<float>(ac);
  }
  peak = blockPeak;
  // how much is still ringing, whatever the phase: each mode's amplitude
  double e = 0;
  for (auto& md : modes)
    if (md.on) e += std::abs(md.tip) * md.amplitude();
  energy = e / aref;
  age += out.frames / sr;
}

// ---------------------------------------------------------------------------
// The global chain: pickup loading, preamp, tremolo, speakers, room, and the
// noises old electronics make.

void ElectricPiano::processGlobal(GlobalContext& ctx, Buffer& io) {
  const auto at = static_cast<ep::Amp>(ctx.choice(ampType));
  const auto& av = ep::ampVoice(at);
  const bool mag = ctx.choice(pickupType) == 0;
  const double fe = std::clamp(2.0 * ctx.get(age) * ctx.get(wElec), 0.0, 1.0);

  // pickup loading: magnetic pickups resonate with the cable; electrostatic
  // ones are high-impedance and lose their lowest octave
  pkLp.set(mag ? av.pickupLp : 9000.f, mag ? av.pickupQ : 0.f);
  pkHp.set(mag ? 30.f : 110.f, 0.f);
  eqLo.set(120, 0, static_cast<float>(ctx.get(bass)));
  eqHi.set(2500, 0, static_cast<float>(ctx.get(treble) - 7 * fe));
  const float spk = static_cast<float>(ctx.get(speaker));
  for (int c = 0; c < 2; ++c) {
    cabHp[c].set(av.hp, av.hpQ);
    cabPk[c].set(av.peak, av.peakQ, av.peakDb);
    cabLp[c].set(av.lp, 0);
  }
  rattleBp.set(900, 1.5f);

  const float g = static_cast<float>(dsp::dbToGain(static_cast<float>(-6 + ctx.get(drive) * 30 + 8 * fe)));
  const float makeup = av.level / std::sqrt(std::max(0.3f, g)) * 1.1f;
  const float bias = av.bias + 0.25f * static_cast<float>(fe);
  const float hard = av.hard;
  const float atRest = ep::clip01(bias, hard);

  // tremolo
  const double depth = ctx.get(tremDepth);
  double rate = ctx.get(tremRate);
  const auto& tr = ctx.transport();
  const bool sync = ctx.on(tremSync);
  Division div = Division::Eighth;
  Division::parse(kDivisions[std::clamp(ctx.choice(tremDiv), 0, 7)], div);
  const double divBeats = std::max(1e-3, div.length(tr));
  const double humHz = ctx.choice(mains) == 0 ? 60.0 : 50.0;
  const double humLevel = fe > 0 ? dsp::dbToGain(static_cast<float>(-100 + 55 * std::sqrt(fe))) / 2.3 : 0.0;
  const double hissLevel = fe > 0 ? dsp::dbToGain(static_cast<float>(-98 + 32 * std::sqrt(fe))) : 0.0;
  const float wet = static_cast<float>(ctx.get(roomMix) * 0.55);
  const double actTarget = ctx.activeVoices() > 0 ? 1.0 : 0.0;

  for (int i = 0; i < io.frames; ++i) {
    // electronics noise follows playing (a gate, so silence is silence)
    activity += (actTarget - activity) * (actTarget > activity ? 0.002 : 2.5 / sr);
    const float noiseGate = static_cast<float>(activity);

    float m = io.left[i];
    m = pkHp.process(pkLp.process(m));
    // mains hum: the fundamental plus rectifier buzz harmonics
    if (humLevel > 0) {
      humPhase += humHz / sr;
      if (humPhase >= 1) humPhase -= 1;
      const double p = ep::kTwoPi * humPhase;
      m += static_cast<float>(humLevel * noiseGate *
                              (std::sin(p) + 0.6 * std::sin(2 * p) + 0.35 * std::sin(3 * p) + 0.2 * std::sin(4 * p) +
                               0.12 * std::sin(5 * p)));
    }
    m = eqHi.process(eqLo.process(m));

    // preamp, at 2x
    float a, b;
    osUp.up(m * g, a, b);
    a = ep::saturate(a, bias, hard, atRest);
    b = ep::saturate(b, bias, hard, atRest);
    m = osDown.down(a, b) * makeup;

    // scratchy pot: the odd dip in level
    if (fe > 0) {
      if (potHold > 0) {
        if (--potHold == 0) potTarget = 1;
      } else if (grng.uniform() < fe * fe * fe * 0.4 / sr) {
        potTarget = 1 - grng.uniform() * 0.7 * fe;
        potHold = static_cast<int>((0.04 + 0.16 * grng.uniform()) * sr);
      }
      potGain += (potTarget - potGain) * 0.004;
      m *= static_cast<float>(potGain);
      // crackle: tiny pops through the amp
      if (grng.uniform() < fe * fe * fe * 8 / sr) {
        crackleEnv = 1;
        crackleAmp = (grng.uniform() < 0.1 ? 4 : 1) * 0.02 * fe * grng.bipolar();
      }
      if (crackleEnv > 1e-4) {
        const float w = grng.bipolar();
        crackHpState = w - crackPrev;   // first difference: a tick
        crackPrev = w;
        m += static_cast<float>(crackleEnv * crackleAmp) * crackHpState * noiseGate;
        crackleEnv *= kCrackle;
      }
      m += static_cast<float>(hissLevel) * hiss.pink() * 2.5f * noiseGate;
    }

    // tremolo (the Suitcase's "vibrato" pans between its two speaker pairs)
    wobble += (grng.bipolar() - wobble) * (2.0 / sr);
    double ph;
    if (sync) {
      ph = (tr.beat + i * tr.bpm / 60.0 / sr) / divBeats;
    } else {
      tremPhase += rate * (1 + 0.6 * fe * wobble) / sr;
      if (tremPhase >= 1) tremPhase -= 1;
      ph = tremPhase;
    }
    const double s = std::sin(ep::kTwoPi * ph);
    float l = m, r = m;
    if (depth > 0) {
      if (av.stereoTrem) {
        l *= static_cast<float>(1 - depth * (0.5 + 0.5 * s));
        r *= static_cast<float>(1 - depth * (0.5 - 0.5 * s));
      } else {
        const double shaped = std::tanh(1.6 * s) / std::tanh(1.6);   // the 200A's slightly squared swing
        const float tg = static_cast<float>(1 - depth * 0.5 * (1 + shaped));
        l *= tg;
        r *= tg;
      }
    }

    // speakers, and a rattling cone on loud low notes when tired
    float cl = cabLp[0].process(cabPk[0].process(cabHp[0].process(l)));
    float cr = cabLp[1].process(cabPk[1].process(cabHp[1].process(r)));
    if (fe > 0) {
      const float ex = std::max(0.f, std::abs(l + r) * 0.5f - 0.35f);
      if (ex > 0) {
        const float rt = rattleBp.process(grng.bipolar()) * ex * static_cast<float>(fe) * 1.5f;
        cl += rt;
        cr += rt;
      }
    }
    l = l + (cl - l) * spk;
    r = r + (cr - r) * spk;

    float rl = l, rr = r;
    room.process(rl, rr);
    const float ac = acoustic[static_cast<std::size_t>(i)];
    io.left[i] = l + rl * wet + ac;
    io.right[i] = r + rr * wet + ac;
  }
  for (auto* f : {&pkLp, &pkHp, &eqLo, &eqHi, &rattleBp}) f->flush();
  for (int c = 0; c < 2; ++c)
    for (auto* f : {&cabHp[c], &cabPk[c], &cabLp[c]}) f->flush();
  acoustic.fill(0);
}

SK_SYNTH(ElectricPiano)
