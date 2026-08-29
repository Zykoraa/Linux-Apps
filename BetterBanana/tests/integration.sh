#!/usr/bin/env bash
# End-to-end test: drives real audio through the running engine and checks the
# numbers coming out of the virtual sources. Requires bb-engine to be running.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CTL="$ROOT/build/bb-ctl"
TMP="$(mktemp -d)"
PASS=0; FAIL=0

command -v paplay >/dev/null || { echo "paplay missing"; exit 1; }
$CTL status >/dev/null 2>&1 || { echo "engine not running"; exit 1; }

# These tests measure absolute levels, so any application feeding one of our
# virtual sinks would be summed with the test tone. Park them all on a real sink
# and put them back afterwards. This has to cover every bb_ sink, not just
# bb_vaio: an application parked elsewhere can be pulled back onto a cable by
# the GUI's auto-routing rules, and a cable may be the ducker's key strip.
FALLBACK=$(pactl list short sinks | awk '$2 !~ /^bb_/ { print $2; exit }')

# "pactl list short sink-inputs" columns: index, sink-id, client, driver, format.
streams_on_ours () {
  local ids
  ids=$(pactl list short sinks | awk '$2 ~ /^bb_/ { print $1 }' | tr '\n' '|' | sed 's/|$//')
  [ -z "$ids" ] && return 0
  pactl list short sink-inputs | awk -v re="^($ids)$" '$2 ~ re { print $1 }'
}

PARKED="$(streams_on_ours | tr '\n' ' ')"
park () {
  [ -z "$FALLBACK" ] && return 0
  for i in $(streams_on_ours); do pactl move-sink-input "$i" "$FALLBACK" 2>/dev/null; done
}
restore_all () {
  for i in $PARKED; do pactl move-sink-input "$i" bb_vaio 2>/dev/null; done
  [ -f "$TMP/before.bbp" ] && $CTL preset load "$TMP/before.bbp" >/dev/null 2>&1
  rm -rf "$TMP"
}
$CTL preset save "$TMP/before.bbp" >/dev/null 2>&1 || true
trap restore_all EXIT
if [ -n "${PARKED// /}" ]; then
  echo "  (parking stream(s) $PARKED off the virtual sinks for the duration)"
  park
  sleep 0.5
fi

python3 - "$TMP" <<'PY'
import struct, math, wave, sys
w = wave.open(sys.argv[1] + '/tone.wav', 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', int(12000*math.sin(2*math.pi*440*i/48000)),
                                          int( 6000*math.sin(2*math.pi*440*i/48000))) for i in range(48000*3)))
w.close()
PY

peak () { # $1 wav -> prints peak of left channel, ignoring edges
python3 - "$1" <<'PY'
import wave, struct, sys
try: w = wave.open(sys.argv[1])
except Exception: print(-1); raise SystemExit
n = w.getnframes()
if n == 0: print(0); raise SystemExit
s = struct.unpack('<%dh' % (n*2), w.readframes(n))
L = s[0::2][int(0.3*48000):]
print(max((abs(x) for x in L), default=0))
PY
}

run () { # $1 label, $2 expected b1 peak, $3 tolerance, $4 expected b2 peak
  parecord -d bb_b1 --file-format=wav --rate=48000 --channels=2 "$TMP/b1.wav" & R1=$!
  parecord -d bb_b2 --file-format=wav --rate=48000 --channels=2 "$TMP/b2.wav" & R2=$!
  sleep 0.4; paplay -d bb_vaio "$TMP/tone.wav"; sleep 0.3
  kill $R1 $R2 2>/dev/null; wait $R1 $R2 2>/dev/null
  local p1 p2 ok
  p1=$(peak "$TMP/b1.wav"); p2=$(peak "$TMP/b2.wav")
  ok=1
  [ "$(( p1 > $2 - $3 && p1 < $2 + $3 ? 1 : 0 ))" = 1 ] || ok=0
  [ "$(( p2 > $4 - $3 && p2 < $4 + $3 ? 1 : 0 ))" = 1 ] || ok=0
  if [ $ok = 1 ]; then echo "  ok    $1  (B1=$p1 B2=$p2)"; PASS=$((PASS+1))
  else echo "  FAIL  $1  B1=$p1 (want ~$2)  B2=$p2 (want ~$4)"; FAIL=$((FAIL+1)); fi
}

reset_state () {
  # Custom strip/bus names change the status columns these assertions parse,
  # so start from the built-in names. The caller's names are restored from the
  # snapshot preset on exit.
  for i in 0 1 2 3 4; do $CTL label strip $i "" ; $CTL strip $i key 0; $CTL strip $i duck 0; done
  $CTL duck off
  for b in A1 A2 A3 B1 B2; do $CTL label bus $b "" ; done
  # Every other strip is silenced and unrouted, so a live microphone or a
  # virtual cable cannot leak into the measured bus.
  for i in 0 1 2 4; do
    for b in A1 A2 A3 B1 B2; do $CTL strip $i bus $b 0; done
  done
  for i in 0 1 2 3 4; do
    $CTL strip $i gain 0; $CTL strip $i mute 0; $CTL strip $i solo 0; $CTL strip $i mono 0
    $CTL strip $i gate 0; $CTL strip $i comp 0; $CTL strip $i aud 0
    $CTL strip $i eq 0 0 0; $CTL strip $i pan 0
  done
  for b in A1 A2 A3 B1 B2; do $CTL bus $b gain 0; $CTL bus $b mute 0; $CTL bus $b mono 0; $CTL bus $b eq 0; done
  $CTL strip 3 bus B1 1; $CTL strip 3 bus B2 0
}

echo "[betterbanana integration]"
reset_state
run "unity gain, VAIO->B1 only"            12000 60 0
$CTL strip 3 bus B2 1
run "matrix: VAIO->B1 and B2"              12000 60 12000
$CTL strip 3 bus B2 0
$CTL strip 3 gain -6.0206
run "strip gain -6.02 dB halves"            6000 60 0
$CTL strip 3 gain 0; $CTL strip 3 mute 1
run "strip mute silences"                       0 60 0
$CTL strip 3 mute 0; $CTL bus B1 gain -12.0412
run "bus gain -12.04 dB quarters"           3000 60 0
$CTL bus B1 gain 0; $CTL strip 3 mono 1
run "strip mono folds L+R to (12000+6000)/2" 9000 90 0
$CTL strip 3 mono 0
# Solo is per bus now, so the soloed strip has to actually feed the bus being
# measured for anything to be silenced there.
$CTL strip 4 bus B1 1; $CTL strip 4 solo 1
run "solo on a strip feeding B1 mutes VAIO there" 0 60 0
$CTL strip 4 solo 0; $CTL strip 4 bus B1 0
run "solo cleared restores VAIO"            12000 60 0
$CTL strip 3 pan -1
run "hard-left pan: L at +3 dB pan law"     16971 200 0
reset_state


# ---- per-bus solo ----------------------------------------------------------
# Solo must silence other strips only on the buses the soloed strip feeds,
# not everywhere, which is what a global solo would do.
echo "[per-bus solo]"
reset_state
$CTL strip 3 bus B1 1; $CTL strip 3 bus B2 1     # VAIO -> both virtual buses
$CTL strip 4 bus B1 1; $CTL strip 4 bus B2 0     # AUX  -> B1 only
$CTL strip 4 solo 1                              # solo AUX (which is silent)
run "solo on B1 mutes VAIO there, B2 untouched" 0 60 12000
$CTL strip 4 solo 0
reset_state

# ---- sidechain ducking -----------------------------------------------------
echo "[sidechain ducking]"
duck_peak () {   # $1 = "key" to also drive the key strip
  park
  parecord -d bb_b2 --file-format=wav --rate=48000 --channels=2 "$TMP/d.wav" & R=$!
  sleep 0.4
  [ "$1" = key ] && paplay -d bb_cable1 "$TMP/tone.wav" &
  paplay -d bb_vaio "$TMP/tone.wav"
  sleep 0.2; kill $R 2>/dev/null; wait $R 2>/dev/null
  python3 - "$TMP/d.wav" <<'PY2'
import wave, struct, sys
w = wave.open(sys.argv[1]); n = w.getnframes()
s = struct.unpack('<%dh' % (n*2), w.readframes(n))[0::2][int(1.2*48000):]
print(max((abs(v) for v in s), default=0))
PY2
}
reset_state
$CTL strip 3 bus B2 1                            # only VAIO reaches B2
$CTL strip 1 key 1                               # Cable 1 is the key
$CTL strip 3 duck -14                            # VAIO drops 14 dB while keyed
$CTL duck on; $CTL duck threshold -34
sleep 1
IDLE=$(duck_peak nokey)
DUCKED=$(duck_peak key)
if [ "$IDLE" -gt 11000 ] && [ "$IDLE" -lt 13000 ]; then
  echo "  ok    ducker idle leaves the signal alone ($IDLE)"; PASS=$((PASS+1))
else
  echo "  FAIL  ducker idle: got $IDLE want ~12000"; FAIL=$((FAIL+1))
fi
# -14 dB of 12000 is 2394; allow for envelope attack at the start of the take.
if [ "$DUCKED" -gt 2000 ] && [ "$DUCKED" -lt 2900 ]; then
  echo "  ok    key signal ducks the target by 14 dB ($DUCKED)"; PASS=$((PASS+1))
else
  echo "  FAIL  ducked: got $DUCKED want ~2394"; FAIL=$((FAIL+1))
fi
$CTL duck off
reset_state

# ---- preset round trip -----------------------------------------------------
# Regression guard: the key parser must match whole keys. sscanf reports
# assigned fields, not literal matches, so a loose matcher lets strip.N.buses
# be parsed as strip.N.gain.
echo "[preset round trip]"
P="$TMP/rt.bbp"
$CTL strip 0 gain -13.5; $CTL strip 0 gate 6.5; $CTL strip 0 comp 3.25
$CTL strip 0 eq 7 -4 2.5; $CTL strip 1 mute 1; $CTL strip 4 gain -21
$CTL bus A2 mono 1; $CTL bus B1 gain -9.5
$CTL preset save "$P" >/dev/null
for i in 0 1 2 3 4; do $CTL strip $i gain 0; $CTL strip $i mute 0; $CTL strip $i gate 0
  $CTL strip $i comp 0; $CTL strip $i eq 0 0 0; done
for b in A1 A2 A3 B1 B2; do $CTL bus $b gain 0; $CTL bus $b mono 0; done
$CTL preset load "$P" >/dev/null
sleep 0.5
chk () {  # $1 label, $2 expected, $3 actual
  if [ "$2" = "$3" ]; then echo "  ok    $1 ($3)"; PASS=$((PASS+1))
  else echo "  FAIL  $1: got '$3' want '$2'"; FAIL=$((FAIL+1)); fi
}
ST=$($CTL status)
chk "strip 0 gain restored"  "-13.5" "$(echo "$ST" | awk '/^HW IN 1/{print $4}')"
chk "strip 0 gate restored"  "6.5"   "$(echo "$ST" | awk '/^HW IN 1/{print $8}')"
chk "strip 1 mute restored"  "1"     "$(echo "$ST" | awk '/^HW IN 2/{print $5}')"
chk "strip 4 gain restored"  "-21.0" "$(echo "$ST" | awk '/^AUX/{print $2}')"
chk "bus A2 mono restored"   "1"     "$(echo "$ST" | awk '/^A2 /{print $4}')"
chk "bus B1 gain restored"   "-9.5"  "$(echo "$ST" | awk '/^B1 /{print $2}')"
reset_state

echo
echo "  $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
