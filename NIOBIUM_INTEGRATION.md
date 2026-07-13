# How this workload was integrated with the Niobium compiler

This note documents what was changed in **this repository** to run the
Fetch-by-Similarity encrypted computation on the **Niobium** compiler/backend
(via the [niobium-client](submission/niobium-client) SDK), and what you need to
know to work with it. It is written for someone new to Niobium — it focuses on
the integration *steps and changes*, not on Niobium's internals.

## What the integration gives you

Niobium **records** the server-side encrypted computation once as a portable
trace, then **replays** that trace — either locally (for debugging) or on the
Niobium backend (for an optimized/accelerated run). You keep the normal OpenFHE
implementation of the workload; Niobium wraps the part you want it to execute.

Because the trace captures the computation *graph* (not the data), a recorded
trace can be **replayed with new keys and new inputs** without re-recording, as
long as the crypto parameters (ring dimension, modulus chain) are unchanged.

Recording uses **hollow mode**: it captures the instruction trace while skipping
the expensive polynomial math, which keeps the recording run cheap on time and
memory. The trade-off is that a recording run does not itself produce a usable
result, so every run — including the first — finishes by replaying the trace to
get the answer. This makes the first (recording) run a **cold start** that costs
more than steady-state replays; see [Running it](#running-it).

## The pieces that changed (and why)

| File | Change |
|------|--------|
| `submission/src/server_encrypted_compute.cpp` | The one file with real integration code. Adds the explicit Niobium record/replay lifecycle around the existing FHE computation, all behind `#ifdef NIOBIUM_COMPILER`. Read its comments for the step-by-step. |
| `submission/CMakeLists.txt` | Compiles `server_encrypted_compute` with `-DNIOBIUM_COMPILER`, against the Niobium-instrumented OpenFHE (`OPENFHE_CPROBES`), linking `libnbfhetch` + the auto-facade library. |
| `harness/run_submission.py` | Adds `--target` (default `local`) and forwards it to the compute step; sets the replay environment (`NBCC_FHETCH_DRIVER` for local, or `NBCC_FHETCH_REPLAY` — the transport forwarder — for a backend target) + the OpenFHE library path. For a backend target it *reads* `NBCC_FHETCH_SERVER` (the worker URL) but never sets it — `scripts/fog submit` supplies that. |
| `submission/niobium-client/scripts/fog` (client SDK) | How a non-local / FOG run reaches a backend: `fog submit` requests a worker from the FOG API and runs your harness against it, so there's no separate replay server to start (see [Running it](#running-it)). An earlier `scripts/start_fhetch_server.py` helper has been removed. |
| `submission/niobium-client` | The Niobium client SDK (git submodule). The integration uses its cooperative auto-tagging mode. |
| `submission/include/params.h`, `submission/src/client_key_generation.cpp` | The `TOY` size now uses the full ring dimension (`ringDim = 65536`) with `HEStd_128_classic` security, the same crypto parameters as all other sizes — instead of its previous reduced ring (`ringDim = 1024`, no security). This keeps recorded traces parameter-compatible across sizes while still using toy-scale data (1000 records, 128-dim) for quick checks. |

## The integration pattern (in `server_encrypted_compute.cpp`)

The important steps, in order:

1. **`init(argc, argv)` then `enable_auto_tagging()`** — first thing in `main`,
   *before* the crypto context is loaded. `init` consumes Niobium flags
   (`--target`); `enable_auto_tagging` lets Niobium capture the crypto context,
   keys, and input ciphertexts automatically as your code deserializes them.
2. **`cache_parameters({...})` + `set_program_info(...)`** — identify the
   computation (here: instance size + count/full mode) so its trace is keyed and
   reused. Must be set before the crypto context loads.
3. **Load** the crypto context, keys, and query the normal OpenFHE way — Niobium
   auto-tags them (and remembers their file paths).
4. **Gate only the *recording* on `is_cache_valid()`, then always replay:**
   - **record** (no trace yet): `start()` → `enable_hollow_mode(true)` → run the
     FHE computation in **hollow mode** (the heavy polynomial math is skipped, so
     recording is fast and low-memory, but the value it leaves in `out` is **not**
     a valid result) → `enable_hollow_mode(false)` → `probe("result", out)` →
     `stop()`.
   - **cache hit** (trace exists): skip recording entirely — run **no** FHE ops.
   - **then, in both cases:** `replay()` (refreshes any changed input/key files
     and runs the trace on the target) → `result(cc, "result", out)`. Replay is
     the only path that yields a correct result — a hollow record computes nothing
     usable, and a cache hit runs no FHE at all.
5. **Serialize `out`** — always the ciphertext reconstructed by `replay()`/`result()`.

Rules worth remembering: cache parameters before the context load; **all FHE ops
inside the recording gate** (a cache hit runs zero ops); hollow mode must be
**off** for `probe()`/`stop()`; same crypto parameters across record and replay
(new key/data values are fine).

## Running it

There are two ways to run, and both use the same harness and the same workload
code — only `--target` differs: **locally** against an in-tree simulator (for
debugging, no account), and on the **managed FOG** against Niobium's FPGA (the
customer path).

### One-time setup

1. **macOS transport TLS.** Before building, `export
   OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3` (Intel Macs:
   `/usr/local/opt/openssl@3`). Without it the client's HTTPS transport builds
   **without TLS** and the FOG upload fails; confirm `TLS enabled (OpenSSL 3.x)`
   in the build's config log.
2. **A fog-capable client.** This repo's committed submodule pin predates the
   `scripts/fog` CLI, so bump it to `main` once after cloning:
   ```bash
   cd submission/niobium-client && git fetch origin main && git checkout origin/main
   make sync                       # nested openfhe / fhetch / haze / json
   cd ../..
   ```
   Confirm `submission/niobium-client/scripts/fog` now exists. (Only needed for a
   FOG run; a `--target local` run works with the pinned client.)
3. **FOG access** (FOG runs only). Get an account, then point the CLI at the
   **beta** endpoint and log in:
   - `~/.fog/config` → `api_url = https://api.beta.niobium.co`. The CLI defaults
     to prod (`api.niobium.co`), which **404s on `/jobs/`** — beta is required.
   - `submission/niobium-client/scripts/fog login` — stores your API token.
   - macOS: `pip3 install certifi` — the python CLI needs a CA bundle for TLS.

### Local — in-tree simulator (debugging, no account)
```bash
python harness/run_submission.py 0 --target local
```
`--target local` replays via the in-tree `fhetch_driver` — intentionally slow and
unoptimized, but it validates correctness with no account and no network. The
workload code is identical when you switch to the FOG.

### Managed FOG — replay on Niobium's FPGA (the customer path)
```bash
export OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3      # macOS: transport TLS
submission/niobium-client/scripts/fog submit \
    python harness/run_submission.py 0 --target FOG --opt-level O3
```
`scripts/fog submit` **wraps** the harness: it POSTs a job to the FOG API
(`{mode, target}`), long-polls until a worker is assigned, exports the worker URL
+ token (`NBCC_FHETCH_SERVER` / `NBCC_FHETCH_TOKEN`), then `exec`s your harness.
The harness only ever sets `NBCC_FHETCH_REPLAY` (the transport forwarder), so the
assigned worker flows straight through to the transport client. A **single
invocation records the trace locally, then replays it on the FOG** — there is no
separate record step.

`--target FOG` does double duty: the FOG API reads it to select the pinned stable
FPGA (you never name a device), and the harness reads it as "non-local → use the
forwarder." Any other non-local value is shipped to whatever `NBCC_FHETCH_SERVER`
points at (default `http://127.0.0.1:9443`), for advanced self-hosting.

> **`--opt-level O3` is mandatory** for the FPGA and is the harness default: at
> `O0` the replay skips Memory Squash and overflows the slot allocator. It's
> forwarded end-to-end to the backend.

### Notes on the FOG run

- **Cold start — the first run costs more.** On a cache miss the first run does
  **both**: it records the trace (hollow mode — cheap on math/memory, but it still
  builds and writes the full instruction trace) **and then** replays it to produce
  the result. Later runs hit the cache and **only replay** (Niobium refreshes the
  changed keys/inputs automatically). Expect the first run — or the first after
  changing the computation or crypto parameters, which forces a re-record — to be
  noticeably slower.
- **Record once, replay with new keys.** Keep the recorded trace directory between
  runs; delete it to force a fresh record after changing the computation or crypto
  parameters.
- **A FOG run is upload-bound.** It is dominated by the **HTTPS upload** of the
  tagged keys + ciphertexts, not the FPGA compute. How long that takes depends on
  your payload size and uplink, so measure it on your own connection rather than
  assuming a figure — even the toy size isn't instant. A better-connected host (or
  a smaller instance) is the main lever.

## Build dependency

`scripts/build_task.sh` / the harness build first builds the niobium-client
submodule (its instrumented OpenFHE + `libnbfhetch` + auto-facade + transport),
then builds this workload against it. A `--target local` run needs only that. A
**`--target FOG`** run additionally needs a fog-capable client (the bump step in
[Running it](#running-it)) and an approved FOG account — the FOG worker provides
the backend, so there is no server for you to start.
