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
| `harness/run_submission.py` | Adds `--target` (default `local`) and forwards it to the compute step; sets the replay environment (`NBCC_FHETCH_DRIVER` for local, `NBCC_FHETCH_REPLAY`/`NBCC_FHETCH_SERVER` for a backend target) + the OpenFHE library path. |
| `scripts/start_fhetch_server.py` | Standalone helper to start the replay **server** for a non-local target, wired to an external compiler checkout. (Temporary — meant to move into the compiler repo later.) |
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

```bash
# Local (default): replays via the in-tree fhetch_driver simulator.
python harness/run_submission.py 0 --target local

# Backend target: ships the trace to the Niobium compiler. Start a server first
# (see scripts/start_fhetch_server.py) and select the target.
python harness/run_submission.py 0 --target <backend>
```

- **`--target local` is for debugging only — it is intentionally slow and
  unoptimized** (a software FHETCH simulator running on your machine). Use it to
  validate correctness, then switch the target to run on the Niobium backend; the
  workload code does not change.
- **Cold start: the first run costs more.** On a cache miss the first run does
  **both** — it records the trace (in hollow mode: cheap on math and memory, but
  it still builds and writes the full instruction trace) **and then** replays that
  trace to produce the result. Later runs hit the cache and **only replay**
  (Niobium refreshes the changed keys/inputs automatically). So expect the first
  run after a fresh record — or after changing the computation or crypto
  parameters, which forces a re-record — to be noticeably slower than the
  steady-state replays that follow.
- **Record once, replay with new keys:** keep the recorded trace directory
  between runs; delete it to force a fresh record (and thus another cold start)
  after changing the computation or crypto parameters.

## Build dependency

`scripts/build_task.sh` / the harness build first builds the niobium-client
submodule (its instrumented OpenFHE + `libnbfhetch` + auto-facade), then builds
this workload against it. A backend (`--target`) run additionally needs a
reachable Niobium compiler (`nbcc_fhetch_replay`); `start_fhetch_server.py`
wires one up.
