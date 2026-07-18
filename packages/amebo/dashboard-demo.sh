#!/usr/bin/env bash
# dashboard-demo.sh - boot amebo + a subscriber, seed data, and hold the admin
# dashboard open so you can watch the fragments client (static/dg.js) live:
#   - the 3s stats poll ticking the counters,
#   - the debounced live-search box (type slowly: skeleton shimmer + in-flight
#     request cancellation as you keep typing),
#   - delivered climbing as the aproko worker fires webhooks to the subscriber.
# A background trickle publishes one event every 4s so the poll visibly moves.
# Ctrl-C tears everything down.
#
#   ./dashboard-demo.sh                 # dashboard at http://127.0.0.1:8080/
#   AMEBO_PORT=9090 ./dashboard-demo.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DRAGON="${DRAGON:-$ROOT/build/dragon}"
[ -x "$DRAGON" ] || { echo "dragon not found at $DRAGON; build it or set DRAGON="; exit 1; }
BIN="$(mktemp -d)"
SEC="amebo-demo-secret"
export AMEBO_PORT="${AMEBO_PORT:-8080}"
export SUBSCRIBER_PORT="${SUBSCRIBER_PORT:-19000}"
export AMEBO_ADMIN_KEY="${AMEBO_ADMIN_KEY:-demo-admin-key}"
AMEBO="http://127.0.0.1:$AMEBO_PORT"

echo "building amebo + subscriber ..."
"$DRAGON" build "$HERE/amebo.dr"      -o "$BIN/amebo"      -I "$HERE" || exit 1
"$DRAGON" build "$HERE/subscriber.dr" -o "$BIN/subscriber"           || exit 1

"$BIN/subscriber" > "$BIN/sub.log"   2>&1 & SUBPID=$!
"$BIN/amebo"      > "$BIN/amebo.log" 2>&1 & AMPID=$!
TRICKLE=""
cleanup() { kill "$SUBPID" "$AMPID" $TRICKLE 2>/dev/null; wait 2>/dev/null; rm -rf "$BIN"; echo; echo "stopped."; }
trap cleanup EXIT INT TERM

for _ in $(seq 1 50); do curl -fsS "$AMEBO/health" >/dev/null 2>&1 && break; sleep 0.1; done

# Registration goes through the admin key: the control plane is gated since
# the strict-ACL work (an unauthenticated register is a 403).
echo "seeding applications / action / subscription ..."
curl -s -o /dev/null -X POST "$AMEBO/v1/applications" -H "x-amebo-admin-key: $AMEBO_ADMIN_KEY" -H 'content-type: application/json' \
  --data-binary '{"application":"orders","host":"127.0.0.1","port":0,"secret":"amebo-demo-secret"}'
curl -s -o /dev/null -X POST "$AMEBO/v1/applications" -H "x-amebo-admin-key: $AMEBO_ADMIN_KEY" -H 'content-type: application/json' \
  --data-binary "{\"application\":\"fulfillment\",\"host\":\"127.0.0.1\",\"port\":$SUBSCRIBER_PORT,\"secret\":\"fulfillment-secret\"}"
curl -s -o /dev/null -X POST "$AMEBO/v1/actions" -H "x-amebo-admin-key: $AMEBO_ADMIN_KEY" -H 'content-type: application/json' \
  --data-binary '{"action":"order.placed","application":"orders","schema":{"type":"object","required":["order"],"properties":{"order":{"type":"integer","minimum":1}}}}'
curl -s -o /dev/null -X POST "$AMEBO/v1/authorizations" -H "x-amebo-admin-key: $AMEBO_ADMIN_KEY" -H 'content-type: application/json' \
  --data-binary '{"action":"order.placed","subject_type":"service","subject_name":"fulfillment","read":true}'
curl -s -o /dev/null -X POST "$AMEBO/v1/subscriptions" -H "x-amebo-admin-key: $AMEBO_ADMIN_KEY" -H 'content-type: application/json' \
  --data-binary '{"subscription":"sub-0","application":"fulfillment","action":"order.placed","handler":"/hook","max_retries":5}'

sign() { printf '%s' "$1" | openssl dgst -sha256 -hmac "$SEC" 2>/dev/null | awk '{print $NF}'; }
publish() { curl -s -o /dev/null -X POST "$AMEBO/v1/events" -H "x-amebo-signature: $(sign "$1")" --data-binary "$1"; }

echo "publishing 8 seed events ..."
for i in $(seq 1 8); do
  publish "{\"action\":\"order.placed\",\"deduper\":\"seed-$i\",\"payload\":{\"order\":$i},\"metadata\":{\"source\":\"demo\"}}"
done

# trickle so the 3s poll visibly moves while you watch
( n=100; while true; do sleep 4; publish "{\"action\":\"order.placed\",\"deduper\":\"live-$n\",\"payload\":{\"order\":$n}}"; n=$((n+1)); done ) & TRICKLE=$!

echo
echo "  amebo dashboard:  $AMEBO/   (sign in: admin / admin)"
echo "  - KPI tiles + every chart repaint from /dash/metrics.json every 5s"
echo "  - open a gist: the drawer shows the subscriber's actual response body"
echo "  - 'delivered' climbs as aproko delivers to the subscriber"
echo "  Ctrl-C to stop."
echo
wait "$AMPID"
