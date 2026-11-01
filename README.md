# Electric Piano

A physically modelled electric piano built on
[synthkit](https://github.com/10d6/synthkit). It runs in the browser and as a
VST3 / AU plugin. It covers tine pianos (Rhodes-style) and reed pianos
(Wurlitzer-style), the faults those instruments pick up over the years, and a
few things the real ones can't do.

## Playing it

- **In the browser:** every PR gets a preview link (posted on the PR), and
  `main` is at the repo's GitHub Pages site.
- **In your DAW:** each PR's CI run has a **plugin-windows**, **plugin-macos**
  and **plugin-linux** download (under *Checks → CI → Summary → Artifacts*).
  Unzip it and follow `INSTALL.md` inside. Tagging a version (`v0.2.0`)
  publishes them as a release.

The mod wheel brings in tremolo on most presets (the *Sustainer Swells*
preset uses it for the sustainer). CC 64 is the sustain pedal, CC 67 the
dampers, CC 70 drive and CC 74 treble.

## How it makes its sound

Nothing is sampled. Each key is a small physical system, and most of the
character comes out of the physics by itself:

| Part | Model | What you hear from it |
|---|---|---|
| **Hammer** | a mass on a nonlinear felt/neoprene spring (Hunt–Crossley contact) that feels the tine or reed recoil | harder hits are shorter, so they're brighter as well as louder; bass hammers stay on longer than treble ones |
| **Tine + tonebar** | the tine's cantilever modes (1 : 6.27 : 17.6), with its fundamental coupled to the tonebar; the pair's normal modes are solved per note | the bell "ping" at the attack; long sustain when the tonebar is in tune with its tine, and slow beating when it isn't |
| **Reed** | a weighted steel reed's modes (1 : 7.1 : 19.6), lighter than its hammer | the quick, woody attack and shorter sustain |
| **Magnetic pickup** | flux as a bell-shaped function of the tine tip's position; the output is its rate of change | centred tine: bell-like 2nd harmonic. Offset: fuller fundamental, and *bark* as hard hits swing across the pole piece |
| **Electrostatic pickup** | capacitance as a smoothed step as the reed enters its slot in the charged plate | odd harmonics and the reedy, nasal bite; a growl when played hard |
| **Dampers** | felt that lands after a delay and squeezes the decay rates, with a small thump of its own | the release, and (when worn) notes that ring on or buzz |
| **Electronics** | pickup loading, bass/treble, a 2× oversampled preamp, tremolo, speakers, a small room | Stage (into a Twin), Suitcase (stereo "vibrato" panning between speaker pairs), 200A (its own amp, small speakers), Direct |

The **Mechanism** and **Pickup** are separate switches, so you can also put a
reed on magnets or a tine against an electrostatic plate.

## Age and wear

**Age** scales every fault, and each fault knob sets how prone this
instrument is to that one (at the default 0.5, Age 1 means fully worn). The
**Serial** number decides which keys have which faults. The same serial is
always the same instrument, and turning Age up wears the same keys further.

| Fault | What it does |
|---|---|
| Tuning | per-key drift, the odd key way out (a loose tuning spring or lost reed solder), tonebars off their tines |
| Voicing | pickups knocked out of line or to different distances: uneven tone and level |
| Hammer tips | hardened, grooved tips: clackier, brighter, less velocity-sensitive |
| Dampers | thin felt, late dampers, dampers that don't seat (notes ring on and buzz) |
| Action | uneven key response, keys that bounce and double-strike, a near-dead key, more key noise |
| Rattles | loose tonebar screws or reed clamps that buzz on big swings |
| Electronics | mains hum (60 or 50 Hz), hiss, crackle, a scratchy pot, a tired preamp (duller, more distorted), tremolo wobble, a rattling speaker cone |

The **wear map** (in Condition) shows each key's faults, as the engine
computes them. Click a key to have it serviced (its faults removed, saved with
the patch). *New instrument* picks another serial.

Hum and hiss follow playing (they fade a couple of seconds after the last
note) so a stopped track is silent.

## Twists

| | |
|---|---|
| Partial Ratio | stretches the overtone ratios (the bell): glassy, gamelan-ish or harmonic |
| Sustain | multiplies every decay time |
| Shimmer | detunes and couples the tonebar harder: a chorus inside each note |
| Sustainer | feeds each tine's motion back into it, like an EBow on the tines: endless, swelling notes (try it on the mod wheel) |

The kit's two LFOs, a drawn shape and an 8-slot mod matrix can reach the
hammer hardness, pickup alignment and distance, drive, tremolo, sustain and the
sustainer, so the voicing itself can move.

## Building

```
git clone --recursive <this repo>
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/electricpiano_render --check . --out renders     # every check; WAVs into renders/
tools/demos.sh build/electricpiano_render demo-out       # musical phrases (demos/) as MP3s
python3 tools/harmonics.py build/electricpiano_render '{"mech.type":1,"pickup.type":1}'   # spectra per note
```

The browser page and the plugins are built the same way as in synthkit: see
[`synthkit/API.md`](synthkit/API.md) §11–12.

## Layout

| | |
|---|---|
| `synth.json` | identity (name, IDs, plugin codes) and kit features |
| `src/Physics.h` | hammer, modes, tine/tonebar coupling, pickups, per-key wear |
| `src/Electronics.h` | oversampling, preamp clipping, amp and speaker voicings, the room |
| `src/Synth.cpp` | parameters, the voice (one per key, re-struck in place), the global chain |
| `presets/` | factory presets (`init.json` is what a new instance starts on) |
| `tests/` | render scenarios checked in CI |
| `demos/` | musical phrases for listening (not run in CI) |
| `tools/` | analysis: harmonics per note, spectrograms, demo renders |
| `ui/layout.json`, `ui/custom.js`, `ui/style.css` | the panel layout and the wear map |
| `synthkit/` | the kit, as a pinned submodule |
