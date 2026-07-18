#!/usr/bin/env bash
# e2e.sh - build amebo + a mock subscriber, drive the whole broker over HTTP
# under STRICT per-action authorization: register application/action/subscription,
# grant the subscriber READ, publish schema-valid events, and prove signed
# envelope delivery plus every rejection path (auth, schema, dedupe, signature).
#
#   ./e2e.sh [N]        (default 10 events)   DRAGON=/path/to/dragon to override
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DRAGON="${DRAGON:-$ROOT/build/dragon}"
[ -x "$DRAGON" ] || { echo "dragon not found at $DRAGON; build it or set DRAGON="; exit 1; }
BIN="$(mktemp -d)"
PUB_SECRET="amebo-demo-secret"        # orders (action owner) secret
SUB_SECRET="fulfillment-secret"       # fulfillment (subscriber) secret
# High ports so a dev service on 8080/9000 never collides with the test.
export AMEBO_PORT="${AMEBO_PORT:-18080}"
export SUBSCRIBER_PORT="${SUBSCRIBER_PORT:-19000}"
export AMEBO_ADMIN_KEY="${AMEBO_ADMIN_KEY:-e2e-admin-key}"
ADMIN_KEY="$AMEBO_ADMIN_KEY"
AMEBO="http://127.0.0.1:$AMEBO_PORT"
SUB="http://127.0.0.1:$SUBSCRIBER_PORT"
N="${1:-10}"

echo "building ..."
"$DRAGON" build "$HERE/amebo.dr"      -o "$BIN/amebo"      || exit 1
"$DRAGON" build "$HERE/subscriber.dr" -o "$BIN/subscriber" || exit 1

"$BIN/subscriber" > "$BIN/sub.log"   2>&1 & SUBPID=$!
"$BIN/amebo"      > "$BIN/amebo.log" 2>&1 & AMPID=$!
trap 'kill $SUBPID $AMPID 2>/dev/null; wait $SUBPID $AMPID 2>/dev/null; rm -rf "$BIN"' EXIT

for _ in $(seq 1 50); do curl -fsS "$SUB/health"   >/dev/null 2>&1 && break; sleep 0.1; done
for _ in $(seq 1 50); do curl -fsS "$AMEBO/health" >/dev/null 2>&1 && break; sleep 0.1; done
echo "subscriber: $(curl -s $SUB/health)   amebo: $(curl -s $AMEBO/health)"

sign() { printf '%s' "$2" | openssl dgst -sha256 -hmac "$1" 2>/dev/null | awk '{print $NF}'; } # sign <secret> <body>

post() { # post <path> <body> -> http code   (unauthenticated)
    curl -s -o "$BIN/last" -w '%{http_code}' -X POST "$AMEBO$1" \
         -H 'content-type: application/json' --data-binary "$2"
}
admin_post() { # admin_post <path> <body> -> http code   (superadmin via admin key)
    curl -s -o "$BIN/last" -w '%{http_code}' -X POST "$AMEBO$1" \
         -H "x-amebo-admin-key: $ADMIN_KEY" -H 'content-type: application/json' --data-binary "$2"
}
signed_post() { # signed_post <secret> <app> <path> <body> -> http code   (service principal)
    curl -s -o "$BIN/last" -w '%{http_code}' -X POST "$AMEBO$3" \
         -H "x-amebo-application: $2" -H "x-amebo-signature: $(sign "$1" "$4")" \
         -H 'content-type: application/json' --data-binary "$4"
}

echo "registering application/action over the API ..."
r1="$(admin_post /v1/applications '{"application":"orders","host":"127.0.0.1","port":0,"secret":"amebo-demo-secret"}')"
r2="$(admin_post /v1/applications "{\"application\":\"fulfillment\",\"host\":\"127.0.0.1\",\"port\":$SUBSCRIBER_PORT,\"secret\":\"fulfillment-secret\"}")"
# action registered by its OWNER (orders signs with its own secret) -> owner becomes implicit admin
abody='{"action":"order.placed","application":"orders","schema":{"type":"object","required":["order"],"properties":{"order":{"type":"integer","minimum":1}}}}'
r3="$(signed_post "$PUB_SECRET" orders /v1/actions "$abody")"
echo "applications: $r1 $r2   action(owner-signed): $r3   (expect 201s)"

# auth rejections on the control plane
rnoadmin="$(post /v1/applications '{"application":"sneaky","host":"x","port":1,"secret":"s"}')"
sbody='{"subscription":"sub-0","application":"fulfillment","action":"order.placed","handler":"/hook","max_retries":3}'
rsub_denied="$(signed_post "$SUB_SECRET" fulfillment /v1/subscriptions "$sbody")"   # no READ yet
echo "register app w/o admin key -> $rnoadmin (expect 403)   subscribe w/o READ -> $rsub_denied (expect 403)"

# grant the subscriber READ on the action, then it may subscribe
gbody='{"action":"order.placed","subject_type":"service","subject_name":"fulfillment","read":true}'
rgrant="$(admin_post /v1/authorizations "$gbody")"
r4="$(signed_post "$SUB_SECRET" fulfillment /v1/subscriptions "$sbody")"
echo "grant fulfillment READ -> $rgrant (expect 200)   subscribe after grant -> $r4 (expect 201)"

# registration rejections: bad shape (422) and duplicate (409) - both via admin key
rbad="$(admin_post /v1/applications '{"application":"nameless"}')"
rdup="$(admin_post /v1/applications '{"application":"orders","host":"x","port":1,"secret":"s"}')"
echo "invalid application -> $rbad (expect 422)   duplicate application -> $rdup (expect 409)"

publish() { # publish <body> -> http code   (orders is the owner: legacy owner-signed path, has WRITE)
    curl -s -o "$BIN/last" -w '%{http_code}' -X POST "$AMEBO/v1/events" \
         -H 'content-type: application/json' -H "x-amebo-signature: $(sign "$PUB_SECRET" "$1")" --data-binary "$1"
}

echo "publishing $N schema-valid events (as owner) ..."
ok=0
for i in $(seq 1 "$N"); do
    body="{\"action\":\"order.placed\",\"deduper\":\"d${i}\",\"payload\":{\"order\":${i}},\"metadata\":{\"source\":\"e2e\"}}"
    code="$(publish "$body")"
    [ "$code" = "201" ] && ok=$((ok+1))
done
echo "published: $ok/$N accepted (201)"

# authorization rejection: fulfillment has READ but not WRITE -> may not publish
nowrite="$(signed_post "$SUB_SECRET" fulfillment /v1/events '{"action":"order.placed","deduper":"nw-1","payload":{"order":7}}')"
echo "publish without WRITE -> $nowrite (expect 403)"

# schema rejection: signed correctly, but the payload violates the action schema
badschema="$(publish '{"action":"order.placed","deduper":"bad-1","payload":{"order":"nope"}}')"
badmin="$(publish '{"action":"order.placed","deduper":"bad-2","payload":{"order":0}}')"
echo "wrong-type payload -> $badschema, below-minimum payload -> $badmin (expect 422 422)"
unknown="$(publish '{"action":"order.cancelled","deduper":"bad-3","payload":{}}')"
dup="$(publish '{"action":"order.placed","deduper":"d1","payload":{"order":1}}')"
tampered="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$AMEBO/v1/events" \
        -H 'x-amebo-signature: deadbeef' \
        --data-binary '{"action":"order.placed","deduper":"bad-4","payload":{"order":4}}')"
echo "unknown action -> $unknown (expect 422)   duplicate deduper -> $dup (expect 409)   tampered -> $tampered (expect 401)"

echo "waiting for aproko to deliver ..."
delivered=""
for _ in $(seq 1 40); do
    delivered="$(curl -s "$AMEBO/v1/events" | grep -o '"delivered": *[0-9]*' | grep -o '[0-9]*')"
    [ "$delivered" = "$N" ] && break
    sleep 0.2
done

echo "amebo:      $(curl -s $AMEBO/v1/events)"
# Delivery is proven server-side: amebo marks a gist delivered only on a 2xx,
# and the subscriber 2xx's only a correctly signed, well-formed amebo envelope
# {action, metadata, payload}.
badhook="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$SUB/hook" \
        -H 'x-amebo-signature: deadbeef' --data-binary '{"order":999}')"
echo "tampered webhook -> subscriber HTTP $badhook (expect 401)"

pass=1
[ "$r1$r2$r3$r4" = "201201201201" ] || pass=0
[ "$rnoadmin" = "403" ] && [ "$rsub_denied" = "403" ] && [ "$rgrant" = "200" ] || pass=0
[ "$nowrite" = "403" ] || pass=0
[ "$rbad" = "422" ] && [ "$rdup" = "409" ] || pass=0
[ "$ok" = "$N" ] && [ "$delivered" = "$N" ] || pass=0
[ "$badschema" = "422" ] && [ "$badmin" = "422" ] && [ "$unknown" = "422" ] || pass=0
[ "$dup" = "409" ] && [ "$tampered" = "401" ] && [ "$badhook" = "401" ] || pass=0

if [ "$pass" = "1" ]; then
    echo "PASS - strict ACLs: registered via API, granted READ, published $N, delivered $N signed envelopes, all auth/schema/dedupe/signature rejects correct"
else
    echo "FAIL (reg=$r1$r2$r3$r4 rnoadmin=$rnoadmin rsub_denied=$rsub_denied rgrant=$rgrant nowrite=$nowrite rbad=$rbad rdup=$rdup ok=$ok delivered=$delivered badschema=$badschema badmin=$badmin unknown=$unknown dup=$dup tampered=$tampered badhook=$badhook)"
    exit 1
fi
