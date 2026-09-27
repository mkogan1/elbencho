# AGENTS.md — elbencho

Distributed storage benchmark for files, objects, and block devices (optional CUDA/GDS/S3).
C++17, Make-based build, GPL-3.0-only. Upstream: https://github.com/breuner/elbencho

## Layout

| Path | Role |
|------|------|
| `source/` | Core: `Main.cpp`, `Coordinator`, `ProgArgs`, `Statistics`, HTTP service |
| `source/workers/` | `LocalWorker` / `RemoteWorker` I/O threads and shared state |
| `source/toolkits/` | Helpers (S3, net, random, offsets, strings, …) |
| `bin/` | Built `elbencho` binary |
| `docs/` | Usage help, CSV/result docs, k8s/slurm examples |
| `tools/` | Chart/summarize helpers, `test-examples.sh` |
| `external/` | Vendored/fetched deps (e.g. AWS SDK when S3 enabled) |
| `packaging/` | RPM/DEB packaging |
| `contrib/` | Extra tooling (e.g. storage_sweep) |

## Build

```bash
make help                    # options and feature flags
make -j $(nproc)             # default build → bin/elbencho
make BUILD_DEBUG=1 -j $(nproc)
make S3_SUPPORT=1 -j $(nproc)   # pulls large AWS SDK; slow first build
make clean                   # object files
make clean-all               # also externals; required when toggling features
make deb | make rpm          # packages under packaging/
```

- C++17 (`CXX_FLAVOR=c++17`), default compiler `g++`.
- CUDA / GDS auto-enable when headers/libs are present; override with `CUDA_SUPPORT` / `CUFILE_SUPPORT`.
- After changing optional features, always `make clean-all` before rebuilding.

## Coding conventions

- Indent with **tabs**, not spaces (match existing `.cpp`/`.h`).
- New/edited source files: SPDX header at top:
  ```
  // SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
  // SPDX-License-Identifier: GPL-3.0-only
  ```
- Headers: include guards (`COMMON_H_`, `WORKERS_WORKER_H_`, …), not `#pragma once`.
- Prefer existing patterns: `ProgArgs` for CLI/config, `Worker` hierarchy for I/O, toolkit classes under `source/toolkits/`.
- Keep CLI long option names short (see comments in `ProgArgs.h`: ~12–16 chars for help layout).
- Warnings already enabled in the Makefile (`-Wall -Wextra` …); do not silence them casually.

## Docs & CLI help

- User-facing help text lives under `docs/usage/` (`help.md`, `help-s3.md`, …).
- Regenerate/sync usage docs via `tools/generate-usage-docs.sh` when changing help output.
- Result column meaning: `docs/result-columns-explanation.md`; CSV fields: `docs/csv-docs.md`.

## Testing

```bash
# Quick smoke tests (may need root for loop block devices)
tools/test-examples.sh [-b] [-d] [-m] <BASEDIR>
```

- Prefer a disposable directory as `BASEDIR`.
- `-b` skips block-device tests (no root); `-d` / `-m` skip distributed / multi-file cases.
- After code changes, rebuild then run relevant smoke tests before claiming success.

## S3 RDMA (`--s3rdma`, branch `wip-s3rdma`)

Build the lab binary with the classic S3 client. `S3_AWSCRT=1` does not apply this path.

```bash
nice make -j $(nproc) S3_SUPPORT=1 S3_AWSCRT=0 S3RDMA_SUPPORT=1
```

`make clean-all` first when toggling `S3_SUPPORT` / `S3RDMA_SUPPORT`. Client env: `CUFILE_ENV_PATH_JSON=/etc/cuobj.json`. Startup line: `S3 RDMA fabric connected (cuObject).`

| Path | Role |
|------|------|
| `source/toolkits/S3RdmaTk.cpp` | Shared process-wide `cuObjClient`; each worker has its own registered buffer. `cuObjGet` / `cuObjPut` invoke the SDK HTTP request through a callback. |
| `source/toolkits/S3RdmaTk.h` | `x-amz-rdma-*` header names and callback interface |
| `source/toolkits/S3RdmaMonitor.cpp` | Captures RDMA reply headers from SDK responses for transfer validation |
| `source/toolkits/S3Tk.cpp` | Forces HTTP response checksum validation to `WHEN_REQUIRED` for RDMA |
| `source/workers/LocalWorker.cpp` | GET always sends a bounded `Range`. Multipart RDMA failures and interruption attempt to abort the upload before propagating the original exception. |

The normal S3 SDK handles authentication, headers, retries, and HTTP requests. The object payload moves outside the HTTP body, so `--s3rdma` forces unsigned payload signing and skips SDK response-body checksum validation. Missing RDMA confirmation is an error unless `--s3ignoreerrors` is selected. Concurrent calls on different registered buffers are supported; cuObject does not use the `execution.parallel_io` setting.

GET data direction is server RDMA write (`handleGetObject`, NIC TX on the RGW). PUT uses server RDMA read (`handlePutObject`, NIC RX on the RGW). NIC counters cover all traffic on the interface and cannot alone attribute reverse traffic to cuObject.

Lab finding (RGW `10.40.68.55`, 4 MiB objects): `-N` is per worker, so increasing `-t` also increases the dataset. At `-t 40 -n 1`, reducing `-N 250` (~39.1 GiB) to `-N 8` (1.25 GiB) restored ~20,025 MiB/s with the same thread count. This supports a cache/backing-I/O explanation; the exact cache layer was not established. Server-side `mmfsd` RDMA activity was observed, while the client's RDMA read-request counter stayed at zero. Do not treat the earlier apparent 32-thread cliff as a cuObject concurrency limit. The original client implementation performed equally well after removing the four experimental follow-up commits.

RGW must finish its RDMA write before returning HTTP GET success; that server fix is independent of the dataset finding. The DCI pool is created at server initialization, so changing `rgw_cuobj_num_dcis` requires an radosgw restart.

RGW tree: `/mnt/nvme0n1p1/src-git/ceph--mk--wip_cuobj`, branch `wip-rgw-s3rdma01`; GET completion fix: `13b7870d3b1`. `HANDOVER.md` and `CLAUDE.md` contain earlier diagnostic notes, including hypotheses superseded by these findings.

## Agent guidance

- Prefer small, focused diffs that match surrounding style.
- Do not commit unless asked; do not force-push or rewrite history unless asked.
- Avoid editing `external/` or committing large fetched SDK trees.
- Do not add markdown docs the user did not request (this file is the exception when initializing agent config).
- When changing CLI flags or phases, update `ProgArgs` and the corresponding `docs/usage/` help as needed.
