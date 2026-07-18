#!/usr/bin/env bash
# loadtest.sh - build amebo, then fire N concurrent signed publishes and validate.
#
#   ./loadtest.sh [N] [CONCURRENCY]     (defaults: 300 requests, 20 in flight)
#
# Each request is a unique event signed with HMAC-SHA256 over the exact body
# bytes, as an Amebo producer signs, and every payload is validated against the
# action's JSON Schema on the hot path. We check every response is 201, that a
# tampered signature is rejected 401, an invalid payload is rejected 422, and
# the server-side event count matches.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DRAGON="${DRAGON:-$ROOT/build/dragon}"
[ -x "$DRAGON" ] || { echo "dragon not found at $DRAGON; build it or set DRAGON="; exit 1; }
BIN="$(mktemp -d)"
SECRET="amebo-demo-secret"
export AMEBO_PORT="${AMEBO_PORT:-18080}"
URL="http://127.0.0.1:$AMEBO_PORT"
N="${1:-300}"
CONC="${2:-20}"

echo "building ..."
"$DRAGON" build "$HERE/amebo.dr" -o "$BIN/amebo" || exit 1

"$BIN/amebo" > "$BIN/amebo.log" 2>&1 & SRV=$!
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; rm -rf "$BIN"' EXIT

up=""
for _ in $(seq 1 50); do curl -fsS "$URL/health" >/dev/null 2>&1 && { up=1; break; }; sleep 0.1; done
[ -n "$up" ] || { echo "server did not come up:"; cat "$BIN/amebo.log"; exit 1; }
echo "health: $(curl -s "$URL/health")"

post() { curl -s -o /dev/null -w '%{http_code}' -X POST "$URL$1" -H 'content-type: application/json' --data-binary "$2"; }
echo "registering ..."
r1="$(post /v1/applications '{"application":"orders","host":"127.0.0.1","port":0,"secret":"amebo-demo-secret"}')"
r2="$(post /v1/actions '{"action":"order.placed","application":"orders","schema":{"type":"object","required":["n"],"properties":{"n":{"type":"integer"}}}}')"
echo "application: $r1   action: $r2   (expect 201s)"
[ "$r1$r2" = "201201" ] || { echo "FAIL (registration)"; exit 1; }

publish() {
    local i="$1"
    local body="{\"action\":\"order.placed\",\"deduper\":\"d${i}\",\"payload\":{\"n\":${i}}}"
    local sig
    sig="$(printf '%s' "$body" | openssl dgst -sha256 -hmac "$SECRET" 2>/dev/null | awk '{print $NF}')"
    curl -s -o /dev/null -w '%{http_code}\n' -X POST "$URL/v1/events" \
        -H 'content-type: application/json' -H "x-amebo-signature: ${sig}" --data-binary "$body"
}
export -f publish
export URL SECRET

echo "firing $N requests, $CONC concurrent ..."
start="$(date +%s%N)"
codes="$(seq 1 "$N" | xargs -P "$CONC" -I{} bash -c 'publish "$1"' _ {})"
end="$(date +%s%N)"

ok="$(printf '%s\n' "$codes" | grep -c '^201$')"
secs="$(awk "BEGIN{printf \"%.3f\", ($end-$start)/1e9}")"
rps="$(awk "BEGIN{printf \"%.0f\", $N/(($end-$start)/1e9)}")"
echo "published: ${ok}/${N} -> 201 in ${secs}s (~${rps} req/s)"

badbody='{"action":"order.placed","deduper":"badpayload","payload":{"n":"not-a-number"}}'
badsig="$(printf '%s' "$badbody" | openssl dgst -sha256 -hmac "$SECRET" 2>/dev/null | awk '{print $NF}')"
invalid="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL/v1/events" \
        -H 'content-type: application/json' -H "x-amebo-signature: ${badsig}" --data-binary "$badbody")"
echo "schema-invalid payload -> HTTP ${invalid} (expect 422)"

bad="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL/v1/events" \
        -H 'x-amebo-signature: deadbeef' \
        --data-binary '{"action":"order.placed","deduper":"tampered","payload":{"n":1}}')"
echo "tampered signature -> HTTP ${bad} (expect 401)"
echo "server reports: $(curl -s "$URL/v1/events")"

count="$(curl -s "$URL/v1/events" | grep -o '"events": *[0-9]*' | grep -o '[0-9]*')"
if [ "$ok" = "$N" ] && [ "$bad" = "401" ] && [ "$invalid" = "422" ] && [ "$count" = "$N" ]; then echo "PASS"; else echo "FAIL (ok=$ok bad=$bad invalid=$invalid count=$count)"; exit 1; fi

