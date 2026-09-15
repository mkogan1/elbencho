# CLAUDE.md — elbencho (this fork)

Continuation brief for agents and humans on another machine. Read this first, then
`AGENTS.md` (short coding rules) and `HANDOVER.md` (how to port `--s3fastput` /
`--s3fastget` into a *different* AWS SDK C++ app).

Upstream: https://github.com/breuner/elbencho
License: GPL-3.0-only. C++17, Make-based. Version in `Makefile`: **3.1-12**.

This tree is a **fork** of elbencho used as an S3 client against **Ceph RGW**
(`http://localhost:8000` in the original lab). The interesting work is client-side
CPU cuts, bandwidth shaping, and thread stagger — not a rewrite of the benchmark
core.

---

## Related files in this repo

| File | Role |
|------|------|
| `AGENTS.md` | Short agent rules: layout, make flags, tabs, SPDX, tests |
| `HANDOVER.md` | Port `--s3fastput` / `--s3fastget` CPU-elision into another AWS SDK C++ app (pins SDK **1.11.789**) |
| `docs/usage/s3-bandwidth-shaping.md` | Operator guide for `--limitwrite` + `--thrdelay` |
| `docs/usage/help*.md` | Regenerated CLI help (`tools/generate-usage-docs.sh`) |
| `CLAUDE.md` | This file: full project + fork history so work can resume elsewhere |

Uncommitted at last snapshot (do **not** assume they exist on the other machine unless copied):

- `AGENTS.md`, `HANDOVER.md` (untracked)
- `docs/usage/help.md` (version string `3.1-3` → `3.1-12` only)

Fork commits already on `master` (ahead of stock upstream features):

```
2c8e3f8 s3: cut PUT/GET checksum CPU with unsigned payload and when_required
ff67193 s3: smooth upload traffic shaping and document tuning
```

(`ff67193` also contains the non-`--s3fastget` GetObject 206 / callback `-104` fix.)

---

## What elbencho is

Distributed storage benchmark for **files, objects, and block devices**. Optional
CUDA / GDS / S3 / HDFS. Unified latency, throughput, IOPS. Live stats, CSV/JSON
output, integrity verification, libaio, GPU paths.

Modes (see `BenchPathType` in `source/Common.h`):

- Directory / file / block device
- **S3** (paths become bucket names when `--s3endpoints` is set)
- HDFS, netbench (socket)

Two result columns: **First Done** (stonewall, when the fastest thread finishes)
and **Last Done** (when the slowest thread finishes). See
`docs/result-columns-explanation.md`.

---

## Layout

| Path | Role |
|------|------|
| `source/Main.cpp` | Parse args → `Coordinator::main()` |
| `source/Coordinator.cpp` | Phases, signals, service vs standalone, calls `S3Tk::initS3Global` **after** possible daemonize |
| `source/ProgArgs.h/.cpp` | All CLI / config-file / service-JSON. Long option names: ~12–16 chars |
| `source/Statistics.cpp` | Live + final results, CSV/JSON |
| `source/HTTPService*.cpp` | Distributed **service** mode (`--service`) |
| `source/workers/LocalWorker.cpp` | Actual I/O (file, block, S3 PUT/GET/MPU, …) |
| `source/workers/RemoteWorker.cpp` | Master talking to remote services |
| `source/workers/Worker.cpp` | Phase wait / interrupt |
| `source/workers/WorkerManager.cpp` | Thread spawn |
| `source/toolkits/S3Tk.cpp/.h` | AWS SDK init, client, `S3PacedStreamBuf` / `S3MemoryStream` |
| `source/toolkits/RateLimiter.h` | Per-thread leaky-bucket (no catch-up burst) |
| `source/S3UploadStore.*` | Shared MPU IDs across threads |
| `bin/elbencho` | Built binary |
| `docs/` | Help, CSV, k8s/slurm examples |
| `tools/` | `generate-usage-docs.sh`, `test-examples.sh`, chart/summarize helpers |
| `external/` | Fetched AWS SDK (~1GB) when `S3_SUPPORT=1`. **Do not commit the SDK tree** |
| `external/patches/` | Fork patches applied at SDK clone time |
| `packaging/` | RPM/DEB |
| `contrib/` | Extra tooling (storage_sweep) |

~83 C++ files under `source/`. The S3 hot path is almost entirely
`LocalWorker.cpp` (very large) + `S3Tk.*` + `ProgArgs.*`.

---

## Build (this lab’s command)

```bash
# Default (no S3):
make -j $(nproc)          # → bin/elbencho

# THIS FORK’S USUAL BUILD (libcurl S3Client, not CRT):
nice make S3_SUPPORT=1 S3_AWSCRT=0 -j $(nproc)

make BUILD_DEBUG=1 -j $(nproc)
make clean                # objects only
make clean-all            # objects + externals; REQUIRED when toggling S3/CUDA/CRT
make help                 # all feature flags
make deb | make rpm
```

- C++17 (`CXX_FLAVOR=c++17`), default `g++`.
- `S3_SUPPORT=1` clones AWS SDK **1.11.789** (`AWS_REQUIRED_TAG` in
  `external/prepare-external.sh`) into `external/aws-sdk-cpp` and builds a combined
  static lib `libaws-sdk-all.a`. First S3 build is slow.
- `S3_AWSCRT=0` (Makefile default): classic `Aws::S3::S3Client` + libcurl.
- `S3_AWSCRT=1`: `Aws::S3Crt::S3CrtClient`. **Not** how this lab binary is built.
  `--s3targetgbps` is parsed always, but only **applied** under `#ifdef S3_AWSCRT`.
- CUDA / GDS auto-enable when headers/libs exist; override with `CUDA_SUPPORT` /
  `CUFILE_SUPPORT`.
- After changing optional features: always `make clean-all` then rebuild.
- Install: `make install` → `/usr/local/bin/elbencho` (`INST_PATH`). Confirm which
  binary you are running (`which elbencho` vs `bin/elbencho`).

RHEL/CentOS deps (from README): `boost-devel cmake gcc-c++ git libaio-devel
libcurl-devel libuuid-devel make numactl-devel openssl-devel zlib-devel`.

`elbencho --version` prints compiled features (`s3`, `s3crt`, cuda, …).

---

## Runtime architecture (mental model)

```
main
  → ProgArgs (boost::program_options + optional config file)
  → Coordinator
       → S3Tk::initS3Global()     # MUST be before worker threads; once per process
       → WorkerManager.prepareThreads()
       → runBenchmarks() phases   # MKDIRS / WRITE / READ / RM… coordinated
            LocalWorker::run()
              waitForNextPhase()
              applyThreadStartDelay()   # --thrdelay
              switch(benchPhase) → file/block/S3 ops
```

- **Coordinator** owns phase order (`Coordinator::runBenchmarks`).
- **LocalWorker** threads wait on a bench ID, then run one phase. `--thrdelay`
  sleeps **after** that notify, **once per phase**, not per object.
- S3: typically **one `S3Client` per worker** (round-robin `--s3endpoints`).
  `--s3clientsingle` shares one client (recommended for CRT builds).
- `Aws::InitAPI` / `curl_global_init` must run **before worker threads**.
  `ShutdownAPI` does **not** reset the “already initialized” flag — init once per
  process. In service mode, init happens **after** daemonize.
- Interrupt: Ctrl-C → workers check `isInterruptionRequested`. Long sleeps
  (`RateLimiter`, `--thrdelay`) are sliced (~50 ms) so they abort cleanly.
- Distributed: `--service` HTTP server + master `--hosts`. ProgArgs is serialized
  via `getAsPropertyTreeForService` / `setFromPropertyTreeForService`. Fork
  options `--thrdelay`, `--limitwrite`, `--s3fastget` **are** in that tree.
  `--phasedelay` is coordinator-only and is **not** the template for `--thrdelay`.

Phase names (`source/Common.h`): `WRITE`, `READ`, `MKDIRS`/`MKBUCKETS`, `RMOBJECTS`,
`HEADOBJ`, `LISTOBJ`, `MPUCOMPL`, …

---

## Coding conventions

- Indent with **tabs**, not spaces.
- New/edited source: SPDX header

  ```
  // SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
  // SPDX-License-Identifier: GPL-3.0-only
  ```

- Headers: include guards (`TOOLKITS_S3TK_H_`), not `#pragma once`.
- CLI long names: keep short (comments in `ProgArgs.h`: ~12 chars with params,
  ~16 without) so help columns fit.
- Warnings already on (`-Wall -Wextra` …); do not silence casually.
- Prefer existing patterns: `ProgArgs` for flags, `Worker` hierarchy for I/O,
  toolkits under `source/toolkits/`.
- After changing help strings: `tools/generate-usage-docs.sh` (needs a built
  `bin/elbencho`).
- Do not edit `external/` except patches under `external/patches/` and
  `prepare-external.sh`. Never commit the fetched SDK.
- Do not commit unless asked. No force-push / history rewrite unless asked.

---

## CLI map (this fork)

### Dataset / threads (typical S3)

| Flag | Meaning |
|------|---------|
| `-w` | WRITE phase |
| `-r` | READ phase |
| `-t N` | Worker threads |
| `-n` / `-N` | dirs / files (S3: prefix dirs / objects per dir) |
| `-s` | Object/file size (suffixes: `8m`, `64m`) |
| `-b` | Block / MPU part size |
| `--mkdirs` | Create buckets/dirs first |
| `--iodepth` | Async depth; S3 MPU/GET go async if `> 1` |
| `--timelimit` | Seconds per phase (0 = off) |
| `--live1n` `--liveint 1000` | One-line live stats every 1s |

When `--s3endpoints` is set, positional paths are **bucket names**.

### Fork: stagger + bandwidth

| Flag | Meaning |
|------|---------|
| `--thrdelay <ms>` | Uniform random sleep `[0, ms]` per LocalWorker **once after each phase sync**. Default 0. Units are **milliseconds** (like `--liveint`), not seconds like `--phasedelay`. |
| `--limitwrite <bps>` | **Per-thread** write cap. Suffixes work (`3m` = 3 MiB/s). Aggregate ≈ `threads * limitwrite`. |
| `--limitread <bps>` | Same for reads. GET is still paced **between range/blocks**, not mid-stream. |

`RateLimiter` is leaky-bucket, **no catch-up** after slow I/O. Thread-safe
(mutex around schedule; sleep outside lock) so async MPU parts share one limiter
per worker.

**S3 PUT mid-transfer shaping:** when `--limitwrite` is set, `useS3UploadStreamRateLimit`
is true. `makeS3UploadBodyStream()` wraps the body in `S3MemoryStream` +
`S3PacedStreamBuf`, which paces **reads** of the upload body in **64 KiB** chunks
(`S3PacedStreamBuf::PACE_CHUNK_SIZE`). The whole-part `funcRWRateLimiter` wait is
**skipped** on that path so the part is not double-counted.

**Historical bug:** old limiter only waited *between* ops. A 16 MiB part with
`--limitwrite 10k` still sent 16 MiB at wire speed then slept — useless vs a
~1000 MiB/s NVMe. Mid-transfer stream pacing is the fix.

### Fork: S3 CPU cuts

| Flag | Turns on | Client CPU removed | Does **not** remove |
|------|----------|--------------------|---------------------|
| `--s3fastput` | `--s3unsigned` + `--s3nocompress` | SigV4 **payload SHA-256**; SDK request compression | Flexible checksums if `--s3chksumalgo`; **RGW ETag MD5**; TLS |
| `--s3unsigned` / `--s3sign=2` | `PayloadSigningPolicy::Never` | Same payload SHA-256 (`UNSIGNED-PAYLOAD`) | Same |
| `--s3nocompress` | `UseRequestCompression::DISABLE` | Client gzip of request body | Server compression |
| `--s3fastget` | Body → `/dev/null` + `AWS_RESPONSE_CHECKSUM_VALIDATION=when_required` | Copy into app buffer; SDK hash of GET body when server sends `x-amz-checksum-*` | Range/206; server work |
| `--s3chksumalgo` | Explicit flexible checksum | — | **Adds** CRC CPU back |

**Always on in this fork (not gated on `--s3fastput`):**

```
AWS_REQUEST_CHECKSUM_CALCULATION=when_required
```

set in `S3Tk::initS3Global()` **before** `Aws::InitAPI()`, `overwrite=0` so a user
env can still force `when_supported` (chunked / trailer checksums + more CPU).

`--s3fastget` **only** sets `AWS_RESPONSE_CHECKSUM_VALIDATION=when_required`.
Without it, SDK default `when_supported` still hashes GET bodies when the server
sends checksum headers. Desired behavior (user, 2026-08-11): normal GET checksums
on; skip only with `--s3fastget`.

`--s3sign`: `0` RequestDependent (SDK default: often UNSIGNED on HTTPS, **signed
body on HTTP**), `1` Always, `2` Never. Default in ProgArgs is `0`.

**You need the AWS SDK patch for Never to work**, especially on **HTTP** (local
RGW) and SDK ≥ ~1.11.4xx. Upstream: [aws-sdk-cpp#3297](https://github.com/aws/aws-sdk-cpp/issues/3297)
(`Never` ignored after flexible-checksum changes). Historical: HTTP forced payload
signing even when policy was `Never`.

Patch: `external/patches/aws-sdk-cpp-unsigned-payload-never.patch`
Applied by `apply_awssdk_patches()` in `external/prepare-external.sh` **before**
the “lib already built” fast path (so a new patch invalidates `libaws-sdk-all.a`).

Verify on the wire:

```
x-amz-content-sha256: UNSIGNED-PAYLOAD
```

If you see a 64-hex SHA-256, `Never` is not taking effect.

### Other S3 flags worth knowing

| Flag | Notes |
|------|--------|
| `--s3endpoints` | `[http(s)://]host[:port]`, comma-separated; env `S3_ENDPOINT_URL` |
| `--s3key` `--s3secret` | Also env. Dummy lab keys were `b2345678901234567890` / 40-char secret |
| `--s3region` | |
| `--s3log` `--s3logfile` | AWS SDK log (0–6). Use to confirm UNSIGNED-PAYLOAD |
| `--s3clientsingle` | One client for all threads (CRT recommendation) |
| `--s3targetgbps` | Default **100**. **No-op unless `S3_AWSCRT=1`**. CRT uses it to size connection count (not a per-TCP-conn cap). Per **S3 client instance**. Default elbencho is 100 vs AWS/NIXL CRT default 10. NIXL [PR 1769](https://github.com/ai-dynamo/nixl/pull/1769) is the same knob; elbencho already has it for CRT builds |
| `--s3objprefix` | Object name prefix; can include random marks |
| `--s3ignoreerr` | Ignore PUT/GET errors (stress) |

---

## Source map (fork features)

| Piece | Where |
|-------|--------|
| `--s3fastput` → sign=Never + nocompress | `ProgArgs.cpp` ~1309 |
| `--s3unsigned` → sign=Never | `ProgArgs.cpp` ~1315 |
| Help text for fastput/get/unsigned/sign/limit/thrdelay | `ProgArgs.cpp` option descriptions |
| Env vars before `InitAPI` | `S3Tk::initS3Global()` |
| Client: compression + `PayloadSigningPolicy` | `S3Tk::initS3Client()` |
| CRT `throughputTargetGbps` | `S3Tk.cpp` inside `#ifdef S3_AWSCRT` |
| GET `/dev/null` vs `S3MemoryStream` | `LocalWorker::s3ModeDownloadObject` / `…Async` |
| Skip GET buffer alloc | `LocalWorker::allocIOBuffer()` |
| `--thrdelay` | `LocalWorker::applyThreadStartDelay()` after `waitForNextPhase` |
| Upload body factory + pacing | `LocalWorker::makeS3UploadBodyStream` |
| Enable stream pacing | `initPhaseFunctionPointers()` WRITE + `--limitwrite` |
| Dual-mode streambuf (GET write + PUT read) | `S3PacedStreamBuf` in `S3Tk.h` |
| Leaky bucket | `RateLimiter.h` |
| SDK Never patch | `external/patches/aws-sdk-cpp-unsigned-payload-never.patch` |
| Patch at clone | `external/prepare-external.sh` `apply_awssdk_patches()` |
| Service JSON | `getAsPropertyTreeForService` / `setFromPropertyTreeForService` |
| Shaping operator doc | `docs/usage/s3-bandwidth-shaping.md` |

S3 PUT implementations in `LocalWorker.cpp`:

- `s3ModeUploadObjectSinglePart`
- `s3ModeUploadObjectMultiPart` / `…Async`
- `s3ModeUploadObjectMultiPartShared` / `…Async` (shared MPU across threads)

GET uses **HTTP Range** (`bytes=off-end`) even for “full” objects split into `-b`
chunks. RGW logging **206 Partial Content** for that is **normal**, not an error
by itself.

---

## Init order (easy to get wrong)

```
1. Parse flags (ProgArgs)
2. setenv(AWS_EC2_METADATA_DISABLED)          // optional, avoids IMDS delay
3. setenv(AWS_REQUEST_CHECKSUM_CALCULATION)   // always when_required unless user overrode
4. setenv(AWS_RESPONSE_CHECKSUM_VALIDATION)   // only if --s3fastget
5. Aws::InitAPI(options)                      // reads those env vars
6. Build ClientConfiguration
     - requestCompressionConfig
     - (optional) requestChecksumCalculation / responseChecksumValidation on config
7. new S3Client(..., PayloadSigningPolicy from --s3sign/--s3fastput, ...)
```

Env must be set **before** `InitAPI()`. Config-object equivalents exist on newer
SDK (1.11.4xx+) but env is more portable.

---

## Bugs already found and fixed in this fork

### 1. `--limitwrite` did not constrain large S3 parts

Limiter ran **between** blocks. One 16 MiB UploadPart with `--limitwrite 10k`
still burst ~16 MiB/s per thread. With `-t 300` that saturates a 1000 MiB/s NVMe
and produces throughput/CPU **waves**.

Fix: schedule-based `RateLimiter` + `S3PacedStreamBuf` xsgetn/underflow pacing
during SDK body reads.

### 2. GET without `--s3fastget` → RGW `206` + `callback failed … -104`

After (1), `S3PacedStreamBuf` only implemented **read** (`xsgetn`/`underflow`)
for uploads. Non-fast GET uses `S3MemoryStream` as the **response output**
stream. Writes hit a read-only buf → curl callback failure `-104` (ECONNRESET)
after a short 206.

206 itself is expected (Range GET). The failure was client-side.

Fix: `xsputn` / `overflow` / dual-mode `seekoff` on `S3PacedStreamBuf`. GET
response writes into the preallocated `ioBuf`. **Do not** use a paced/limited
streambuf that only implements `xsgetn` as a GET sink.

`--s3fastget` hid this because it streams to `/dev/null` (`Aws::FStream`), not
`S3MemoryStream`.

### 3. GET checksums accidentally skipped for everyone

After CPU-elision work, setting `AWS_RESPONSE_CHECKSUM_VALIDATION=when_required`
unconditionally made GETs “too fast” (no body hash). User wanted:

- **without** `--s3fastget`: validate when server sends `x-amz-checksum-*`
  (`when_supported`)
- **with** `--s3fastget`: skip (`when_required`), body discarded anyway

That is the current `initS3Global()` logic.

### 4. Lockstep MPU waves

Equal-sized objects, same start time → throughput/CPU sawtooth. `--thrdelay`
(e.g. **1500** ms, not 15000) staggers phase start. 15000 ms with 400 threads
looked “stuck” because start-up is spread over 15 s **and** `--limitwrite`
makes progress smooth/slow.

### 5. “Stuck” runs

Compute expected wall time: total bytes / aggregate cap. Example:

`-t 400 -n 10 -N 25 -s 8m` ≈ 800 GiB. `--limitwrite 3m` → ~1200 MiB/s aggregate
→ **~11–12 min** minimum. Watch `--live1n --liveint 1000`. Also check bucket
name for accidental non-printables (one paste ended with `U+001F`).

---

## What `--s3fastput` will **not** fix (RGW / server)

| Symptom | Cause | Where |
|---------|--------|--------|
| High CPU in `MD5_Update` / `EVP_DigestUpdate` **on the RGW host** during PUT | RGW ETag MD5 in `RGWPutObj::execute()` | Server: `rgw_debug_skip_put_md5` (benchmark-only) |
| High SHA-256 on the **client** despite `Never` | SDK ignoring `Never` | Apply signer patch; confirm `UNSIGNED-PAYLOAD` |
| GET still hashes with `when_required` | App set `ChecksumMode::ENABLED` or env overwritten | Don’t enable checksum mode on GetObject |
| MPU Complete still “slow” | Composite ETag is MD5 of 16-byte part etags — cheap | Ignore |

`UNSIGNED-PAYLOAD` only replaces the **SigV4 body hash**. The request is still
SigV4-signed (canonical headers + string-to-sign). RGW still computes **MD5 for
ETag** on ordinary PutObject/UploadPart unless the **server** flag is on.

MD5 for ETag is done for **both** single-part PUT and MPU UploadPart (same
`RGWPutObj::execute()`). Complete MPU MD5 is over part etags, not payload.

### RGW change (different git tree)

Implemented in Ceph fork (not this elbencho repo):

- Path (lab): `/mnt/raid0/src-git/ceph--MK--POSIX_01`
- Branch: `rgw-standalone-mark-perf-asio1-mpucache`
- Commit: `c514744389b` — `rgw: add rgw_debug_skip_put_md5 benchmarking option`
- Files: `src/common/options/rgw.yaml.in`, `src/rgw/rgw_op.cc`, `src/rgw/rgw_main.cc`
- `with_legacy: true` was required so `_conf->rgw_debug_skip_put_md5` compiles
  (regenerate `rgw_legacy_options.h`)
- Placeholder ETag is 32 hex zeros (valid for `hex_to_buf` on MPU complete)
- Startup `derr` WARNING when enabled
- **Never production**: breaks ETag integrity, conditional requests, client
  ETag==MD5 checks, some multisite/lifecycle consumers

Details and porting checklist for **client** CPU cuts: `HANDOVER.md`.

---

## Typical lab workload

Backend: local RGW, NVMe ~**1000 MiB/s**. Leave headroom under that or you get
queueing spikes that look like “waves” even with `--thrdelay`.

```bash
# Stable high-concurrency PUT (example)
/usr/local/bin/elbencho \
  --s3endpoints http://localhost:8000 \
  --s3key S3KEY --s3secret S3SECRET \
  -w -t 300 -n 10 -N 25 -s 8m -b 8m \
  --s3fastput --mkdirs \
  --limitwrite 3m \
  --thrdelay 1500 \
  mybucket1

# Short sanity
... -t 32 -n 2 -N 4 -s 8m -b 8m \
  --limitwrite 3m --thrdelay 1500 \
  --live1n --liveint 1000 --timelimit 120 \
  mybucket1
```

Sizing: `-t 300 --limitwrite 3m` ≈ 900 MiB/s aggregate (under 1000).
`-t 400 --limitwrite 3m` ≈ 1200 MiB/s (often too high for that NVMe).

GET: add `-r`. Use `--s3fastget` only when you want discard + skip response
checksum CPU. Integrity / GPU GET must **not** use `--s3fastget`.

---

## Docs & testing

```bash
# Regenerated from the binary:
tools/generate-usage-docs.sh     # docs/usage/help.md, help-s3.md, help-all.md, …

# Smoke (disposable BASEDIR; -b skip blockdev/root; -d skip distributed; -m skip multi-file)
tools/test-examples.sh [-b] [-d] [-m] <BASEDIR>
```

User-facing help lives under `docs/usage/`. CSV fields: `docs/csv-docs.md`.
Charts: `tools/elbencho-chart`, JSON summary: `tools/elbencho-summarize-json`.

After CLI / phase changes: update `ProgArgs` **and** regenerate usage docs.

---

## Costs to skip (client), in usual impact order

```
PUT / UploadPart body
  ├─ SigV4 payload SHA-256          ← UNSIGNED-PAYLOAD / Never  (--s3fastput / --s3unsigned)
  ├─ Flexible checksum (CRC32C…)    ← AWS_REQUEST_CHECKSUM_CALCULATION=when_required (always)
  ├─ Request compression            ← UseRequestCompression::DISABLE (--s3nocompress)
  └─ Content-MD5 (legacy)           ← modern SDK already off by default

GET / GetObject body
  ├─ Copy into app memory           ← /dev/null (--s3fastget)
  └─ Response checksum validation   ← when_required (--s3fastget only)
```

Language map if porting (full table in `HANDOVER.md`): boto3
`payload_signing_enabled`, Go v2 `UnsignedPayloadMiddleware`, CRT
`aws_s3_checksum_config`.

---

## Open / not done (for the next machine)

1. **Port fastput/get into another app** — that was the last explicit product
   ask; `HANDOVER.md` is the brief. Elbencho side is implemented.
2. **CRT build (`S3_AWSCRT=1`)** — not used in this lab. `--s3targetgbps` would
   start mattering; it is **per S3 client**, not per TCP connection. With one
   client per thread (default), aggregate CRT target ≈ `-t * --s3targetgbps`
   only in the sense of each client independently asking CRT for that many
   connections — not a hard bps cap. `--s3clientsingle` → one client, one
   target. NIXL saw 10 → 150 Gbps help and 400 Gbps hurt; tune, don’t max.
3. **GET mid-transfer `--limitread` pacing** — still between Range requests, not
   inside the response stream (`xsputn` is unpaced). PUT is the smoothed path.
4. **`--thrdelay` uniqueness** — random with replacement; ranks XOR into
   Xoshiro seed but delays can collide. Fine for desync.
5. Copy `AGENTS.md` / `HANDOVER.md` / this file if they are still untracked.
6. Do not assume `/mnt/raid0/src-git/ceph--MK--POSIX_01` exists elsewhere.

---

## Agent working rules (repeat)

- Small diffs matching surrounding style.
- Tabs. SPDX. Include guards.
- Rebuild with `nice make S3_SUPPORT=1 S3_AWSCRT=0 -j $(nproc)` unless asked
  otherwise; `make clean-all` when toggling features.
- After code changes that affect S3 GET streams: test **both** with and without
  `--s3fastget` (206 + checksum CPU).
- After limiter/stream changes: test PUT with `--limitwrite` **and** GET without
  fastget.
- Confirm `x-amz-content-sha256: UNSIGNED-PAYLOAD` if touching signing.
- Do not treat RGW 206 on Range GET as a bug by itself.
)
