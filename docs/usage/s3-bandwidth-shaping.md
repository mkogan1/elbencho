# S3 Bandwidth Shaping Guide

This guide explains how to smooth and constrain S3 upload traffic with `elbencho`.

## Key Concepts

- `--limitwrite` is a per-thread limit in bytes/sec.
- Aggregate target throughput is approximately:
  - `threads * limitwrite`
- `--thrdelay` staggers phase starts to avoid all workers starting at the same instant.
- With current stream-based shaping, upload traffic is paced during each transfer (not only
  between multipart parts).

## Practical Sizing

If your backend tops out around 1000 MiB/s:

- `-t 400 --limitwrite 3m` targets about 1200 MiB/s aggregate (often too high).
- `-t 300 --limitwrite 3m` targets about 900 MiB/s aggregate.
- Leave headroom below device/server max to reduce queueing spikes.

## Recommended Tuning Order

1. Pick a per-thread limit that keeps aggregate throughput below backend max.
2. Add `--thrdelay` (for example `1500` to `15000`) to desynchronize worker starts.
3. Keep `-b` at your desired multipart size; shaping now happens during stream upload.
4. Use live stats (`--live1n --liveint 1000`) to verify stable throughput.

## Example Commands

### Stable high-concurrency PUT with cap

```bash
/usr/local/bin/elbencho \
  --s3endpoints http://localhost:8000 \
  --s3key S3KEY \
  --s3secret S3SECRET \
  -w -t 300 -n 10 -N 25 -s 8m -b 8m \
  --s3fastput --mkdirs \
  --limitwrite 3m \
  --thrdelay 1500 \
  mybucket1
```

### Short sanity run with progress and hard stop

```bash
/usr/local/bin/elbencho \
  --s3endpoints http://localhost:8000 \
  --s3key S3KEY \
  --s3secret S3SECRET \
  -w -t 32 -n 2 -N 4 -s 8m -b 8m \
  --s3fastput --mkdirs \
  --limitwrite 3m \
  --thrdelay 1500 \
  --live1n --liveint 1000 --timelimit 120 \
  mybucket1
```

## Troubleshooting

- If a run appears "stuck", compute expected runtime from total data and configured aggregate
  limit; heavy shaping makes progress smoother but less bursty.
- Ensure bucket names and command arguments do not contain accidental non-printable characters.
