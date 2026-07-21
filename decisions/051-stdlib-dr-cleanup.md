# 051 - Stdlib `.dr` Code-Quality Cleanup (TRANSIENT)

Approved. transient. Delete this file once every box below is checked. Work tracker, not permanent doctrine: batch of stdlib code smells from a full read plus the fix for each. When done, delete; history lives in git + `bugs.md`. (`smells.md` tracks user-facing API overlap and stays; this is implementation junk behind the surface.)

I did a full read of all 101 stdlib `.dr` files (~27K lines) and wrote down recurring smells: builtins reimplemented by hand, O(n^2) string building (a direct #1 violation), copy-pasted logic across sibling modules, magic literals, mixed boolean casing, dead code, and two files over the soft split limit. None of these change the language; all are internal cleanup.

Healthy baseline from that pass: zero em-dashes (the ban still holds after chunking the 9000+ line files), no `TODO`/`FIXME` rot, no commented-out dead code blocks, and the small algorithmic modules (`bisect`, `operator`, `math` core, `heapq` core) are clean.

**How to read this:** each theme has an **Audit** block (what the read-through flagged) and a **Progress** block (what I actually did, deferred, or corrected after verifying at runtime). Unchecked audit lines with no progress entry are still open.

## Ground truth (I verified these - do not re-derive)

- `abs` / `min` / `max` are registered builtins (`Sema.cpp:1148-1150`).
- `abs` dispatches by operand type (`CallBuiltins.cpp:647-669`): `f64` -> `dragon_abs_float` (fabs), int -> `dragon_abs_int`, bool widens to i64. So `abs(x)` is correct for both int and float.
- `min`/`max` accept 2+ scalar args and promote int->float on mixed operands (`CallBuiltins.cpp:883-911`). So `min(a, b)` / `max(a, b)` work.
- Therefore every private `_abs_float` / `abs_f` / `_min` / `_min2` is a needless reinvention, not a workaround.
- `str` methods `.strip` / `.lstrip` / `.lower` / `.isdigit` / `.isalpha` / `.isalnum` / `.isspace` exist; `chr(code)` works; `int(s, 16)` and f-string `{x:02x}` hex formatting work; `ch in "chars"` membership works on `str`.

## Cleanup checklist

Ordered by payoff vs risk. Theme 1 and 3 first. Grep for callers before deleting anything.

### Theme 1 - Reinvented builtins (do first)

**Audit**

- `html.dr` - `_code_to_char` 95-branch ladder -> guarded `chr(code)`.
- `drs.dr` - `_is_digit` / `_is_alpha` -> single `ch in "<charset>"` membership test.
- `json.dr` - `_is_digit` -> `ch.isdigit`.
- `tomllib.dr` - dead `_is_digit` delete.
- `json.dr` `_skip_ws`, `tomllib.dr`/`configparser.dr` `_strip` - trim helpers vs position scanners.
- `uuid.dr` - `_hex_char` 16-branch ladder -> `f"{v:x}"`.
- `html.dr` - `_hex_digit_value` 22-branch ladder -> `int(s, 16)` or shared hex helper.
- `math.dr` - `abs_f` pure `fabs` shim (0 callers).
- `statistics.dr` - dead `_abs_float`.
- `reprlib.dr` - `_min(a,b)` -> `min`.
- `textwrap.dr` - `_min2(a,b)` -> `min`.
- `fnmatch.dr` - dead `_to_lower`.
- `string.dr:99-113` - `is_numeric`/`is_alpha`/`is_alnum`/`is_space` thin aliases - drop or keep?

**Progress**

- [x] `html.dr` - `_code_to_char` -> guarded `chr(code)` (kept the 32..126 range guard so the out-of-range `&#code;` fallthrough is preserved).
- [x] `drs.dr` - `_is_digit` / `_is_alpha` -> single `ch in "<charset>"` membership test (`_is_alpha` keeps `_`).
- [x] `json.dr` - `_is_digit` -> `ch.isdigit`.
- [x] `tomllib.dr` - dead `_is_digit` deleted.
- [x] `json.dr` `_skip_ws`, `tomllib.dr`/`configparser.dr` `_strip` - `_skip_ws` is a POSITION scanner (returns an index), NOT a trim, so it can't be `.strip`; flattened its 4-deep nesting to `if ch in " \t\n\r"`. The two `_strip` trims -> `s.strip(" \t\r")` (exact: spaces/tabs/CR, no `\n` - matches prior behaviour; plain `.strip` would over-strip `\n\v\f`).
- [x] `uuid.dr` - `_hex_char` 16-branch ladder -> `f"{v:x}"` (callers only pass nibbles 0-15). `_rand_hex`'s `out=out+c` loop left for Theme 2 (n<=32 chars, immaterial).
- [x] `html.dr` - `_hex_digit_value` 22-branch ladder -> `ord` math (matches json's `_hex_val`). Audit suggested `int(s, 16)` but that's NOT a Dragon builtin (`int` rejects a 2nd base arg - I verified), so `_hex_to_int` stays as the accumulator.
- [x] `math.dr` - `abs_f` deleted; `fabs` extern + `abs` builtin remain.
- [x] `statistics.dr` - dead `_abs_float` deleted.
- [x] `reprlib.dr` - `_min(a,b)` -> `min` at its 4 call sites; helper deleted.
- [x] `textwrap.dr` - `_min2(a,b)` -> `min` at its 1 call site; helper deleted.
- [x] `fnmatch.dr` - dead `_to_lower` deleted.
- [~] `string.dr:99-113` - KEPT. Public module surface with no in-repo callers; removing public API is more than a cleanup and they are harmless. Revisit only if the one-obvious-way audit (`smells.md`) decides to drop them.
- [x] KEEP (documented, did not "fix"): `fractions.dr:25 _abs_int` (used 3x), `math.dr:92 clamp` (legitimate public utility).

Verified: builtins `chr`, `ord`, `min`/`max` (2 scalars), `abs` (int+float dispatch), `.isdigit`, `.strip(charset)`, `ch in "..."`, and f-string `{x:x}` all confirmed at runtime before swapping. All 118 `dr_*` dogfood tests pass; html/json/tomllib/configparser/uuid/drs behaviour spot-checked against expected output.

### Theme 2 - O(n^2) string/bytes building (#1: speed)

**Audit**

Pattern `out = out + ch` in a loop reallocates the whole buffer each step. Fix: accumulate into a `list[str]`/`list[int]` and join once.

- `json.dr` - `escape`/`unescape` and `dumps_*` builders.
- `urllib/parse.dr` - `quote`.
- `http/client.dr` - `raw = raw + chunk` recv loops.
- `uuid.dr` `_rand_hex` - small n but same pattern.
- `textwrap.dr` - `_expand_tabs`, `_replace_whitespace`, possibly `wrap`/`dedent`.
- `database/mysql.dr` - per-byte wire-packet building.
- `mimetypes.dr` - `_table` rebuilt per call.
- `quopri.dr:34-37` - `_quote` nibble slicing.
- `base64.dr:144-148` - redundant per-quad zeroing.

**Progress**

- [x] `json.dr` - `escape`/`unescape` (incl. surrogate-pair path) and `dumps_list_str/int/float/bool` -> `list[str]` + `"".join`/`", ".join`. **Top perf item.** Surrogate decode (`A😀`), all two-char escapes, and array round-trips verified identical. (Legacy `_parse_string_value` left to Out of scope; its `\uXXXX` drop is pre-existing.)
- [x] `urllib/parse.dr` - `quote` -> `list[str]` + `"".join` (matches `unquote` below it). UTF-8 (`café`->`caf%C3%A9`) and safe-set behaviour verified.
- [~] `http/client.dr` - FOLDED INTO Theme 3. Same loops are the HTTP/HTTPS duplication; fixing perf and dedup separately would edit the same lines twice, so `list[str]`+join lands inside the shared `_read_response` helper there.
- [~] `uuid.dr` `_rand_hex` - left as-is: n <= 32 chars, O(n^2) immaterial.
- [x] `textwrap.dr` - `_expand_tabs` and `_replace_whitespace` -> `list[str]`+join. The `wrap`/`dedent` line-builders are bounded by line width (not quadratic in document size) and were left.
- [~] `database/mysql.dr` - DEFERRED. Cannot verify without a live MySQL server (dogfood suite has none); blindly rewriting wire-protocol byte assembly risks silent corruption. Needs server-backed integration test first.
- [x] `mimetypes.dr` - `_table` rebuilt a 28-entry dict per call -> module global populated once via `_ensure_mime_table` (the uuid `_ensure_seeded` idiom; imported module top-level imperative statements do not run, and a multi-line `{...}` literal is impossible because NEWLINE is live inside braces).
- [~] `quopri.dr:34-37` - skipped (minor; hot loops already use `"".join`).
- [~] `base64.dr:144-148` - skipped (micro-opt, no measurable gain).

Verified: `"".join`/`", ".join` are the existing stdlib idiom (re/io/quopri use them). All 118 `dr_*` dogfood tests pass; json/urllib/mimetypes/textwrap behaviour spot-checked identical.

### Theme 3 - Duplication to factor (do first, with Theme 1)

**Audit**

- `os/os.dr:721-938` <-> `os/path.dr` - ~13 path functions byte-for-byte copied.
- `glob.dr:57-140` - `_fnmatch` verbatim copy of `fnmatch._match`.
- `database/base.dr` - three duplicated patterns across mysql/postgres/sqlite.
- Constant-time compare copy-pasted 3x: `argon2id.dr:48`, `hmac.dr:85`, `totp.dr:82`.
- `hashlib.dr:35-204` - algorithm dispatch repeated ~7x (and again in `hmac.dr:18-37`).
- `gcd` in both `fractions.dr:11` and `math.dr:102`.
- Leap-year + `_days_in_month` duplicated across `datetime.dr` and `calendar.dr`.
- zlib/zstd `extern "C"` decls re-declared in `tarfile.dr`, `gzip.dr`, `zstandard/zstandard.dr`.
- `binascii.dr:34-39` <-> `quopri.dr:18-31` - two `_hex_value` decoders.
- `unittest.dr:117-298` - repeated `extra = msg; if extra=="" {...}; self._fail(...)` block in 13 assert methods.
- `warnings.dr:118-143` - `warn`/`warn_explicit` share dedup tail.
- `http/client.dr:108-369` - HTTP/HTTPS near-identical bodies.
- `http/server.dr:121-139, 546-594` - three near-identical header-serialization loops; path-param parse loop duplicated between `Route`/`WsRoute`.
- `argparse.dr:91-118` - long-option and short-option branches near-identical.
- `threading.dr:96-159` - `acquire(blocking, timeout)` dispatch tree copy-pasted across `RWLock`/`Semaphore`.
- `logging.dr:49-88` - `_emit_to_stderr` and `Logger._emit` build the same line.
- `random.dr:16` / `uuid.dr:14` - identical `RAND_MAX` + rand/srand/time externs.
- `bisect.dr:55-81` - `insort_left`/`insort_right` differ only in bisect call.
- `fractions.dr:140` vs `220` - `from_float` reimplements convergent recurrence from `limit_denominator`.

**Progress**

- [x] `os/os.dr` <-> `os/path.dr` - delegated all ~13 copies via `from os.path import` (ADR 042 pattern); folded duplicate `abspath` and doubly-declared `dragon_realpath` extern. Equivalence verified.
- [x] `glob.dr:57-140` - routed through `fnmatch.fnmatchcase`; copy was identical minus comments.
- [ ] `database/base.dr` - still open (see audit list above).
- [x] Constant-time compare - `argon2id` now `from hmac import compare_digest as _ct_equal`. (`totp` keeps its str/ord variant deliberately.)
- [ ] `hashlib.dr` dispatch - still open.
- [x] `gcd` - `fractions` now `from math import gcd`.
- [ ] Leap-year / `_days_in_month` - still open.
- [ ] zlib/zstd extern re-declarations - still open.
- [ ] Shared hex-nibble helper (`binascii` home) - still open.
- [x] `unittest.dr` - one `_fail_with` helper; default built only on the failure branch.
- [x] `warnings.dr` - shared `_emit`; dedup key built lazily inside the once/default branch.
- [ ] `http/client.dr` HTTP/HTTPS dedup - still open (folds Theme 2 recv-loop fix).
- [ ] `http/server.dr` header loops + path-param parse - still open.
- [ ] `argparse.dr` option branches - still open.
- [ ] `threading.dr` acquire dispatch - still open.
- [ ] `logging.dr` line formatting - still open.
- [ ] `random.dr` / `uuid.dr` shared extern module - still open (low).
- [ ] `bisect.dr` insort helpers - still open (low).
- [ ] `fractions.dr` convergent helper - still open (low).

### Theme 4 - `True`/`False` vs `true`/`false` (one mechanical sweep)

**Audit**

- [ ] Pick one casing repo-wide and sweep. 13 files mix both within one file: `io.dr`, `subprocess.dr`, `filecmp.dr`, `quopri.dr`, `argon2id.dr`, `sched.dr`, `calendar.dr`, `http/sessions.dr`, `http/message.dr` (4 vs 44), `http/multipart.dr`, `http/server.dr` (6 vs 64), `database/mysql.dr`, `urllib/robotparser.dr`. Cross-file outliers also disagree (`ipaddress`/`shlex`/`itertools`/`timeit` use `True`/`False`; many others use `true`/`false`).

### Theme 5 - Magic literals

**Audit**

- `socket.dr` - AF_INET/SOCK_* etc. as named consts.
- `threading.dr` - malloc sizes as named consts.
- `datetime.dr` - SECONDS_PER_* / route float contexts through named consts.
- Decimal file modes -> octal (`os`, `shutil`, `tempfile`).
- `os.dr` - `access(path, 0/4/2/1)` -> F_OK/R_OK/W_OK/X_OK consts.
- `io.dr` - fseek uses SEEK_* consts; read/truncate sentinels.
- `http/websocket.dr` opcode mask / close code / recv buffer.
- DB wire constants (`mysql`/`postgres`).
- `json.dr` keyword/number bytes, `http/message.dr` ASCII compares.
- Sakamoto table, configparser truthy set, tempfile `nchars`, argparse argv heuristic, binary-format offsets.

**Progress**

- [x] `socket.dr` - public `AF_INET`/`SOCK_STREAM`/`SOCK_DGRAM`/`SOL_SOCKET`/`SO_REUSEADDR`/`IPPROTO_TCP`/`TCP_NODELAY`/`SHUT_*` consts used in every `socket`/`setsockopt` call.
- [x] `threading.dr` - `_MUTEX_SIZE`/`_RWLOCK_SIZE`/`_COND_SIZE`/`_BARRIER_SIZE`/`_SEM_SIZE`/`_EVENT_FLAG_SIZE` at actual malloc'd values (ABI-preserving); extern comments now read as true-size reference.
- [x] `datetime.dr` - `SECONDS_PER_HOUR`/`SECONDS_PER_MINUTE`, named all `3600`/`60`, routed float contexts through `float(SECONDS_PER_DAY)`/`float(MICROS_PER_SECOND)`. (Hinnant epoch offsets `719468`/`146097` left as-is - deep in civil-date bit-math.)
- [x] Decimal file modes -> octal: `os.dr makedirs` `0o755`, `shutil.dr` `0o755`, `tempfile.dr` `0o700`.
- [x] `os.dr` - `access(path, 0/4/2/1)` now uses in-file `F_OK`/`R_OK`/`W_OK`/`X_OK` consts (12 sites). (`os/path.dr` has no such const and one raw `access(path, 0)` - left.)
- [x] `io.dr` - `fseek` uses in-file `SEEK_END`/`SEEK_SET` consts; `0 - 1` read/truncate sentinels are now `-1` (5 sites).
- [~] `http/websocket.dr` - DEFERRED (hard to verify without live websocket).
- [~] DB wire constants - DEFERRED with the DB drivers.
- [~] `json.dr` keyword bytes, `http/message.dr` ASCII compares - SKIPPED (low value).
- [~] Sakamoto table, configparser truthy set, tempfile `nchars`, argparse argv heuristic, binary offsets - SKIPPED (low value or already commented).

### Theme 6 - Useless / stale / misleading comments

**Audit**

- `heapq.dr` - `_pop_last` wrapper speculation vs reality.
- `csv.dr` - `parse_rows` comment claims newline preservation inside quoted fields.
- `urllib/request.dr` - `_do_request` comment claims close-on-exception.
- `drs.dr:1-15` - header still describes phased rollout history.
- `syslog.dr` - severity functions restate "Log a message at LOG_X".
- Banner lines (`#===---===#`), restating comments in json/configparser/hashlib/uuid/io/stat, `tarfile.dr` ctor comments.

**Progress**

- [x] `heapq.dr` - deleted 14-line `_pop_last` wrapper (body was just `items.pop`); `heappop` calls `heap.pop` directly.
- [x] `csv.dr` - comment rewritten: splits on `\n` first, so newline inside a quoted field is NOT preserved.
- [x] `urllib/request.dr` - comment fixed: does NOT close on exception; caller owns the response.
- [x] `drs.dr:1-15` - header describes current surface, not phased rollout history. (Deeper `1497`/`1595`/`1820` musings left - low value, deep in 1800-line file.)
- [x] `syslog.dr` - tightened 8 severity-function comments; dropped "Log a message at LOG_X" restatement.
- [~] `tarfile.dr` ctor comments - SKIPPED (low value).
- [~] Restating comments in json/configparser/hashlib/uuid/io/stat - SKIPPED (subjective; many already removed where functions changed in Themes 1-2).
- [~] 492 `#===---===#` banner lines - SKIPPED (intentional style; mass-trimming too subjective).

### Theme 7 - Dead code (grep callers first)

**Audit**

- `json.dr` - orphaned `indent_str` + "pretty-print helpers" banner.
- `statistics.dr` - dead `_sorted_int`.
- `os.dr` - `execv_path`/`execvp_path` dead `return r` after unconditional `raise`.
- `database/postgres.dr:388` - `_param_text` unreachable `return ""`.
- `gzip.dr` / `zstandard/zstandard.dr` - write-only `_read_pos`; placeholder-only `ZstdDecompressor` ctor.
- `struct.dr` convenience packers - grep says mixed use.
- `signal.dr` `SIG_DFL`/`SIG_IGN` - grep says referenced?
- `operator.dr:131` `length_hint` default param.
- `subprocess.dr:78` WIFSTOPPED / `http/websocket.dr` unreachable raise.

**Progress**

- [x] `json.dr` - removed orphaned `indent_str` + banner (0 callers; pretty-print doesn't exist).
- [x] `statistics.dr` - removed dead `_sorted_int`.
- [x] `os.dr` - dropped dead `const r`/`return r` after unconditional `raise` (verified `-> int` body ending in `raise` compiles).
- [~] `database/postgres.dr:388` - DEFERRED with DB drivers (no live server in dogfood suite).
- [x] `gzip.dr` / `zstandard/zstandard.dr` - removed write-only `_read_pos` (4 sites) and placeholder-only `ZstdDecompressor` ctor.
- [~] `struct.dr` convenience packers - KEPT. Grep corrected the audit: `pack_u32_le` (3 callers) and `pack_f64_le` (2) are USED; only `pack_u16_le`/`pack_u64_le`/`pack_f64_be` unused. Removing public Dragon API is beyond safe cleanup.
- [~] `signal.dr` `SIG_DFL`/`SIG_IGN` - KEPT. Grep corrected the audit: they ARE referenced.
- [~] `operator.dr:131` `length_hint` default param - KEPT (Python-signature parity).
- [~] `subprocess.dr:78` WIFSTOPPED / `http/websocket.dr` unreachable raise - KEPT (defensive branch / return-flow appeasement).
- [x] (`fnmatch._to_lower`, `tomllib._is_digit`, `statistics._abs_float` - removed in Theme 1.)

### Theme 8 - Oversized files (over the 1500-line stdlib soft-split)

**Audit**

- [ ] `http/server.dr` (1927, near the 2000 hard cap) - extract `Response` + chunk/hex helpers (~`269-622`) into `http/response.dr`; matches the existing `connection.dr`/`message.dr` split precedent.
- [ ] `drs.dr` (1827) - split into `drs/tokenizer.dr` (TK_* consts + char predicates + `_tokenize` + token helpers) and `drs/eval.dr` (`_box_*` + `_eval_*` ladder + interpolation, ~`619-1078`), leaving parser/dumps/Schema in `drs.dr`.

### Misc less-than-ideal (low priority)

**Audit**

- [ ] `statistics.dr:195,237` - `j = len(values) # break` fake-break idiom; use a real `break`.
- [ ] `uuid.dr:154-187` - 4-deep nested `if` to skip dash positions; flatten with named const position set `[8,13,18,23]`.
- [ ] `os.dr:431-498` - `listdir`/`scandir` share readdir loop; flatten `.`/`..` guard; `walk_*` should use `extend`/`for-in` not index-append.
- [ ] `collections/concurrent.dr:36-134` - `ConcurrentList`/`ConcurrentDict` are int-only pass-through wrappers; note limitation or generalize.
- [ ] `ui/ui.dr:128` - `Signal.set` recomputes `len(self._subs)` each loop iteration; hoist. Reconcile `__patch_text`/`_run_effect` dunder-vs-single-underscore naming.
- [ ] `os/path.dr:131` - local `abspath` shadows module function `abspath`; rename the local.

## Out of scope (likely bugs, not cleanup - confirm separately before touching)

These edge past "smell" into possible correctness issues. Triage in `bugs.md`, not here:

- `http/cookiejar.dr:189` - `Max-Age` (relative seconds) stored into the `expires` (absolute timestamp) slot, which `is_expired` compares as absolute.
- `drs.dr:1274` - `local_bindings = bindings` aliases (does not copy) the dict, leaking the for-expression loop variable into the enclosing scope.
- `random.dr:40-114` - `randint`/`randrange` use `rand % range` (modulo bias); `sample` is not a faithful sample (returns first-k in order, truncates when `k > len`).
- `json.dr:301-361` - legacy slice-based `_parse_string_value` silently drops `\uXXXX` escapes (treats `\u` as literal `u`), diverging from `unescape`.
- `drs.dr:825-826,879-880` - mixed int/float arithmetic seeds float accumulators via `_box_to_int`, so a str operand in a float context drops its fraction.

## How to land the sweep

- Each theme is independently shippable; land them as separate commits so the diff stays reviewable and `git bisect` can attribute any regression.
- Re-run the affected test suites after each theme (`dragon_codegen_tests`, the `test/dr/*.dr` dogfood suites). Deletions of "unused" helpers must be backed by a repo-wide grep including `test/` and `examples/`.
- When the last box is checked, **delete this file.** A one-line summary of the sweep belongs in the commit message, not in a lingering ADR.
