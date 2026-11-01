#!/bin/sh
# Render the musical phrases in demos/ through a few presets each, as MP3s.
#   tools/demos.sh build/electricpiano_render out-dir
set -e
R=${1:-build/electricpiano_render}; OUT=${2:-demo-out}; mkdir -p "$OUT"
r() {
  python3 -c "import json; d=json.load(open('demos/$1.json')); d['patch']='$2'; json.dump(d,open('$OUT/.s.json','w'))"
  "$R" "$OUT/.s.json" --out "$OUT/.s.wav" >/dev/null
  ffmpeg -loglevel error -y -i "$OUT/.s.wav" -codec:a libmp3lame -b:a 192k "$OUT/$1--$2.mp3"
}
for p in init suitcase-vibrato mellow-ballad gigged-hard barn-find glass-tines; do r ballad $p; done
for p in wurli-200a wurli-tremolo basement-wurli reed-on-magnets; do r funk $p; done
for p in bark wurli-grit tine-through-plates; do r bark $p; done
r sustainer sustainer-swells
rm -f "$OUT/.s.json" "$OUT/.s.wav"
