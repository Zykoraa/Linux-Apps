#!/usr/bin/env bash
# Run a command against a throwaway BetterBanana that cannot touch the real one.
#
#     tests/isolated.sh                 # runs tests/integration.sh
#     tests/isolated.sh <command...>    # anything else, e.g. a bash shell
#
# integration.sh drives whatever engine is running and moves applications off
# its sinks while it measures, so on a machine whose audio goes through
# BetterBanana it rewires the live mix. This starts a private PipeWire (no
# hardware: a dummy driver clocks it), a WirePlumber with every hardware monitor
# and D-Bus switched off and no saved state, a pipewire-pulse for pactl and
# paplay, and an engine on its own shared-memory segment (BB_SHM). The command
# runs with the environment pointed at all of them. Nothing reaches the live
# graph, its devices, WirePlumber's remembered routing, the mixer's presets, or
# Discord - the engine's stream guard only ever sees the private graph.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT/build/bb-engine"
CTL="$ROOT/build/bb-ctl"
[ -x "$ENGINE" ] && [ -x "$CTL" ] || { echo "build first: make"; exit 1; }
for t in pipewire wireplumber pipewire-pulse; do
  command -v "$t" >/dev/null || { echo "$t missing"; exit 1; }
done

# A unix socket path is limited to 108 bytes, and PipeWire's sits two levels
# below the runtime dir, so the runtime dir must be short.
RUN="$(mktemp -d /tmp/bbt.XXXXXX)"
HOMEDIR="$(mktemp -d)"
SHM="/bbtest-$$.state"
PIDS=()

cleanup () {
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do kill "${PIDS[i]}" 2>/dev/null; done
  wait 2>/dev/null
  rm -f "/dev/shm${SHM:?}"
  rm -rf "${RUN:?}" "${HOMEDIR:?}"
}
trap cleanup EXIT

cat > "$HOMEDIR/pipewire.conf" <<'EOF'
context.properties = {
    core.daemon = true
    core.name   = pipewire-0
    default.clock.rate = 48000
    mem.allow-mlock = false
    support.dbus = false
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access
      args = { access.socket = { pipewire-0 = "unrestricted", pipewire-0-manager = "unrestricted" } } }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-client-device }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-session-manager }
]
context.objects = [
    { factory = spa-node-factory
      args = { factory.name = support.node.driver node.name = Dummy-Driver
               node.group = pipewire.dummy priority.driver = 20000 } }
]
EOF
mkdir -p "$HOMEDIR/.config/wireplumber/wireplumber.conf.d" "$HOMEDIR/state"
# WirePlumber's own building blocks: policy without any hardware, the
# system-wide mixin (no device reservation, portal or logind) and stateless
# (nothing remembered).
cat > "$HOMEDIR/.config/wireplumber/wireplumber.conf.d/50-isolated.conf" <<'EOF'
wireplumber.profiles = {
  isolated = { inherits = [ policy, mixin.systemwide-session, mixin.stateless ] }
}
EOF

# Everything below sees only the private daemons. Unset PIPEWIRE_NODE, which an
# audio app can leave in its children's environment to pin their target.
unset PIPEWIRE_NODE PIPEWIRE_REMOTE PULSE_SERVER
export XDG_RUNTIME_DIR="$RUN" PIPEWIRE_RUNTIME_DIR="$RUN" PULSE_RUNTIME_PATH="$RUN/pulse"
export HOME="$HOMEDIR" XDG_CONFIG_HOME="$HOMEDIR/.config" XDG_STATE_HOME="$HOMEDIR/state"
export DBUS_SESSION_BUS_ADDRESS="disabled:" BB_SHM="$SHM"

pipewire -c "$HOMEDIR/pipewire.conf" >"$HOMEDIR/pipewire.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do [ -S "$RUN/pipewire-0" ] && break; sleep 0.1; done
[ -S "$RUN/pipewire-0" ] || { echo "private pipewire did not start"; cat "$HOMEDIR/pipewire.log"; exit 1; }
wireplumber --profile isolated >"$HOMEDIR/wireplumber.log" 2>&1 & PIDS+=($!)
pipewire-pulse >"$HOMEDIR/pulse.log" 2>&1 & PIDS+=($!)
"$ENGINE" >"$HOMEDIR/engine.log" 2>&1 & PIDS+=($!)

# Ready once pactl answers and the engine's sinks are in the graph.
for _ in $(seq 100); do
  pactl list short sinks 2>/dev/null | grep -q bb_vaio && break
  sleep 0.1
done
pactl list short sinks 2>/dev/null | grep -q bb_vaio || {
  echo "isolated engine did not come up"; tail -5 "$HOMEDIR/engine.log"; exit 1; }

echo "[isolated] pipewire $RUN, shm $SHM"
if [ $# -eq 0 ]; then set -- "$ROOT/tests/integration.sh"; fi
"$@"
