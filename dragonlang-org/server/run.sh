#!/usr/bin/env bash
#
# Dragon site (docs + landing + package registry) - start / stop / restart helper.
# Launches the unified app at src/app.dr.
#
# Usage:
#   dragonlang-org/server/run.sh start      # build + run
#   dragonlang-org/server/run.sh stop       # kill running instance
#   dragonlang-org/server/run.sh restart    # stop + start
#   dragonlang-org/server/run.sh status     # pid + URL
#   dragonlang-org/server/run.sh logs [N]   # tail -f the server log (default 40 lines)
#
# Environment overrides (rare):
#   DRAGON          - dragon binary (default: $REPO/build/dragon)
#   DRAGON_DOCS_BIN - output binary path (default: /tmp/docs_site)
#   DRAGON_DOCS_LOG - log file path     (default: /tmp/docs_site.log)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"        # dragonlang-org/server
SITE="$(cd "$HERE/.." && pwd)"               # dragonlang-org  (content root + server cwd)
REPO="$(cd "$SITE/.." && pwd)"               # repo root (holds build/dragon)

BIN="${DRAGON_DOCS_BIN:-/tmp/docs_site}"
DRAGON="${DRAGON:-$REPO/build/dragon}"
SRC="$SITE/src/app.dr"
LOG="${DRAGON_DOCS_LOG:-/tmp/docs_site.log}"
PORT=2018

# Match the running binary by full-path-exact command line so we don't
# pick up unrelated processes that happen to mention "docs_site".
match() { pgrep -fx "$BIN" 2>/dev/null || true; }

cmd_status() {
    local pid
    pid="$(match)"
    if [ -n "$pid" ]; then
        echo "running   pid=$pid   http://127.0.0.1:$PORT/   log=$LOG"
        return 0
    fi
    echo "not running"
    return 1
}

cmd_stop() {
    local pid
    pid="$(match)"
    if [ -z "$pid" ]; then
        echo "not running"
        return 0
    fi
    echo "stopping  pid=$pid"
    kill "$pid" 2>/dev/null || true
    # Give it half a second to exit cleanly; escalate to SIGKILL if needed.
    for _ in 1 2 3 4 5; do
        [ -z "$(match)" ] && { echo "stopped"; return 0; }
        sleep 0.1
    done
    echo "  did not exit on SIGTERM - sending SIGKILL"
    kill -9 "$pid" 2>/dev/null || true
    sleep 0.2
    [ -z "$(match)" ] && echo "stopped"
}

cmd_start() {
    local pid
    pid="$(match)"
    if [ -n "$pid" ]; then
        echo "already running   pid=$pid   http://127.0.0.1:$PORT/"
        return 0
    fi
    echo "building   $SRC -> $BIN"
    "$DRAGON" build "$SRC" -o "$BIN"
    echo "starting   (cwd=$SITE, log=$LOG)"
    cd "$SITE"
    # Fail-closed secret: the server refuses to start without DRAGON_REGISTRY_SECRET.
    # For dev we generate a stable per-machine key into a gitignored file the first
    # time, so restarts keep sessions valid without ever embedding a known key.
    if [ -z "${DRAGON_REGISTRY_SECRET:-}" ]; then
        SECRET_FILE="$SITE/data/.session-secret"
        mkdir -p "$SITE/data"
        if [ ! -s "$SECRET_FILE" ]; then
            head -c 48 /dev/urandom | base64 > "$SECRET_FILE"
            chmod 600 "$SECRET_FILE"
        fi
        export DRAGON_REGISTRY_SECRET="$(cat "$SECRET_FILE")"
    fi
    # Ed25519 seed (32 bytes, hex) that signs transparency-log checkpoints. Same
    # fail-closed, per-machine-generated pattern as the session secret.
    if [ -z "${DRAGON_LOG_SIGNING_KEY:-}" ]; then
        LOGKEY_FILE="$SITE/data/.log-signing-key"
        mkdir -p "$SITE/data"
        if [ ! -s "$LOGKEY_FILE" ]; then
            head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$LOGKEY_FILE"
            chmod 600 "$LOGKEY_FILE"
        fi
        export DRAGON_LOG_SIGNING_KEY="$(cat "$LOGKEY_FILE")"
    fi
    # The server inherits cwd from this script; app.dr resolves content
    # and static paths relative to the dragonlang-org/ site root.
    nohup "$BIN" >"$LOG" 2>&1 &
    disown
    sleep 0.5
    pid="$(match)"
    if [ -n "$pid" ]; then
        echo "started    pid=$pid   http://127.0.0.1:$PORT/"
    else
        echo "FAILED to start. last 20 log lines:"
        tail -n 20 "$LOG" 2>/dev/null || true
        return 1
    fi
}

cmd_restart() {
    cmd_stop || true
    cmd_start
}

cmd_logs() {
    local n="${1:-40}"
    [ -f "$LOG" ] || { echo "no log file at $LOG"; return 1; }
    tail -n "$n" -f "$LOG"
}

case "${1:-}" in
    start)   cmd_start ;;
    stop)    cmd_stop ;;
    restart) cmd_restart ;;
    status)  cmd_status ;;
    logs)    shift; cmd_logs "${1:-40}" ;;
    *)
        echo "usage: $0 {start|stop|restart|status|logs [N]}"
        exit 2
        ;;
esac
