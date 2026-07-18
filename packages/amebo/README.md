# amebo

An HTTP event-notifications broker, ported from the Python/heaven original to
Dragon. Applications register schema-backed actions and publish HMAC-signed
events against them; amebo validates each payload against the action's JSON
Schema, persists the event, fans out one delivery record (a *gist*) per active
subscriber, and a background worker (`aproko`) POSTs a signed webhook envelope
to each subscriber, retrying with exponential backoff until it sticks.

This is a working core, not the full port yet - see "Status" below.

## Run it

```bash
# from the repo root, after building the compiler into build/dragon
dragon run packages/amebo                 # boots the broker on :8080
# or
build/dragon build packages/amebo/amebo.dr -o /tmp/amebo && /tmp/amebo
```

`AMEBO_PORT` overrides the port (the test scripts run on 18080/19000 so a dev
service on the default ports never collides).

## Try it

```bash
cd packages/amebo
./e2e.sh          # registers app/action/subscription over the API, publishes,
                  # proves signed envelope delivery + every rejection path
./loadtest.sh     # 300 concurrent signed, schema-validated publishes
```

`e2e.sh` proves the whole loop: register over HTTP, publish N signed events,
the `aproko` worker delivers each as a signed `{action, metadata, payload}`
envelope to `subscriber.dr` (a stateless mock consumer that verifies the
signature and the envelope shape). Delivery is confirmed server-side - amebo
marks a gist delivered only on a 2xx, and the subscriber 2xx's only a valid,
well-formed envelope - so `delivered` reaching N proves N signed envelopes were
accepted. Also proven: schema-invalid payloads and unknown actions are rejected
422, duplicate dedupers 409, tampered signatures 401.

## Shape

- `POST /v1/applications` - register an application (name, host, port, secret).
- `POST /v1/actions` - register an action owned by an application, with the
  JSON Schema every payload published against it must satisfy.
- `POST /v1/subscriptions` - subscribe an application's webhook handler to an
  action (with `max_retries`).
- `POST /v1/events` - verify `x-amebo-signature` (HMAC-SHA256 over the raw
  body, with the action owner's secret), validate the payload against the
  action's schema, persist the event (+ optional `metadata`, optional
  `sleep_until` delay in seconds), and insert one gist per active subscriber.
  Duplicate `deduper` -> 409.
- `GET /v1/events` - `{events, delivered}` counters.
- `GET /health` - liveness.
- `aproko` (background `fire` worker) - leases due pending gists, wraps each
  event in the amebo envelope `{action, metadata, payload}`, signs the envelope
  bytes with the **subscriber's** secret, POSTs to `{subscriber host:port}{handler}`
  with `x-amebo-signature` / `x-amebo-event-id` / `x-amebo-delivery-attempt`,
  marks the gist delivered on 2xx, otherwise bumps its retry count and
  reschedules it `min(3600, 10 * 2^(attempt-1))` seconds out. Every attempt is
  recorded in the `deliveries` log: HTTP status, the subscriber's response body
  (or the connection error text), round-trip latency and wire bytes. The gist
  keeps its latest status/response, so the dashboard and the gist drawer always
  show what the subscriber actually answered.

Payload validation uses the stdlib JSON Schema engine (`json.Schema`: an
instance owns a registry - `register(name, schema)` compiles immediately,
`validate(name, payload)` returns a `ValidationResult` with path-and-message
errors, schemas compose via `$ref`; practical Draft-7 subset, see
`test/dr/test_json_schema.dr`). The broker dogfoods it on its own API: the
request-body schemas share a `$ref`'d `name` schema and are registered at
boot into one instance; each publish validates the payload against the
action's registered schema.

In-memory SQLite behind a `threading.Lock` (the HTTP server runs handlers across
OS worker threads, so one shared connection needs serializing). A production
build would use `database.Pool`.

## Dashboard

`/` (behind session login, seeded admin/admin) is a live operations dashboard
rendered with Apache ECharts (vendored at `static/echarts.min.js`, no CDN):

- KPI row: published, delivered, pending, retrying, failed, success rate,
  p95 latency, due-now backlog - kept live by a 5s poll of
  `/dash/metrics.json`.
- Charts over the last hour, per-minute buckets: event flow (published vs
  delivered), delivery outcomes (ok vs failed attempts), latency (avg + max),
  webhook response mix (2xx/3xx/4xx/5xx/no-conn donut), top actions, wire I/O
  (bytes out/in), and spell runs (ok/failed/error).
- Subscriber health table (attempts, failures, success rate, avg latency,
  last status) and the needs-attention table with one-click requeue.
- Charts read the app's CSS variables, so they follow the light/dark toggle;
  series colors are palette-validated for both modes (CVD-safe).

The gists list shows each delivery's last HTTP status and response snippet;
the gist drawer shows the full response body (or the connection error text)
plus the complete per-attempt history - like the Python original, but with
the whole attempt log, not just the newest answer.

The Events page is the broker's ledger. The list carries delivery-progress
meters, computed status (delivered / in flight / attention / no subscribers),
publisher, payload size, spell counts and status/action filters. Each event
opens a full detail page (/events/:id, no modal): KPI tiles, the payload and
provenance (publisher principal and payload bytes are recorded at reception),
a unified lifecycle timeline that merges reception, every spell run and every
delivery attempt chronologically, a per-subscriber delivery table with
one-click requeue, and redelivery of everything undelivered.

The Applications page is an operations surface, not a name list. The list
carries computed health (healthy / degraded / failing / idle / inactive from
the last hour's delivery success), role, traffic counts and an activity
sparkline per app. Each application's detail page is a command center: KPI
tiles, three live per-app charts (publish activity, webhook consumption,
consumption latency), delivery routes with per-subscription outcome counts
and pause/resume, recent deliveries with response bodies, owned actions with
volumes, a credential card (masked secret, copy, one-click broker-generated
rotation, last-rotated from the audit trail), a per-app audit slice, requeue
of all failed deliveries, and a kill switch: deactivating an application
blocks its publishes AND pauses webhook deliveries to it (queued gists resume
on reactivation - the aproko worker checks the app's active flag).

## Status

Working: API-driven registration, schema-validated signed publish, fan-out,
envelope delivery with retry backoff, end-to-end signed webhook delivery with
a real subscriber, per-attempt delivery telemetry (status, response body,
latency, bytes), and the live ECharts operations dashboard. Passes a
concurrent publish load test (300/300) and an RSS-stability probe over 2000
full deliveries.

Not yet (roadmap toward parity with the Python original):

- Dead-lettering (`dead_at`) once retries exhaust, and a replay (`regists`)
  endpoint. Exhausted gists stay visible (failed + needs-attention) and can be
  requeued from the dashboard, but there is no dedicated replay API.
- Action versioning and deprecated/retired status checks.
- PostgreSQL backend (`FOR UPDATE SKIP LOCKED` + `LISTEN/NOTIFY`).
