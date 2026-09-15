# Handover: elbencho `--s3fastput` / `--s3fastget` CPU-elision

Port these client-side S3 CPU cuts into another app that uses the **AWS SDK for C++** (elbencho pins **1.11.789**). Same knobs exist in other SDKs (boto3, aws-sdk-go, CRT S3) under different names — map the **costs**, not the flag names.

This document is only about **client CPU**. Server-side ETag/MD5 on PutObject/UploadPart (e.g. Ceph RGW) is **not** skipped by these flags. That requires a server option such as `rgw_debug_skip_put_md5`.

---

## What the flags actually do

`--s3fastput` is a convenience alias. `--s3fastget` is GET-side. They are independent.

| Flag | Turns on | Client CPU it removes | Does **not** remove |
|------|----------|----------------------|---------------------|
| `--s3fastput` | `--s3unsigned` + `--s3nocompress` | SigV4 **payload SHA-256** over the PUT/UploadPart body; SDK **request compression** | Flexible checksums if you set `--s3chksumalgo`; RGW/server MD5 for ETag; TLS |
| `--s3unsigned` / `--s3sign=2` | `PayloadSigningPolicy::Never` | Same payload SHA-256 (`x-amz-content-sha256=UNSIGNED-PAYLOAD`) | Same as above |
| `--s3nocompress` | `UseRequestCompression::DISABLE` | Client gzip of the request body | Server compression |
| `--s3fastget` | `/dev/null` response body + `AWS_RESPONSE_CHECKSUM_VALIDATION=when_required` | Copy of GET body into the app buffer; SDK hashing of GET body when the server sends `x-amz-checksum-*` | Range/206 handling; server work |

**Always on in elbencho (not gated on `--s3fastput`):**

```
AWS_REQUEST_CHECKSUM_CALCULATION=when_required
```

That stops the SDK from adding flexible checksums (`x-amz-checksum-*` / trailer / chunked streaming signature) unless the app **explicitly** requests an algorithm. Port this even if you do not add a “fastput” flag.

---

## Costs to skip (in order of usual impact)

```
PUT / UploadPart body
  ├─ SigV4 payload SHA-256          ← UNSIGNED-PAYLOAD / Never
  ├─ Flexible checksum (CRC32C…)    ← AWS_REQUEST_CHECKSUM_CALCULATION=when_required
  ├─ Request compression            ← UseRequestCompression::DISABLE
  └─ Content-MD5 (legacy)           ← modern SDK already off by default
                                      (see aws-sdk-cpp docs/MD5ChecksumFallback.md)

GET / GetObject body
  ├─ Copy into app memory           ← stream to /dev/null (or discard streambuf)
  └─ Response checksum validation   ← AWS_RESPONSE_CHECKSUM_VALIDATION=when_required
```

`UNSIGNED-PAYLOAD` only replaces the **SigV4 body hash**. The request is still SigV4-signed (canonical headers + string-to-sign). RGW still computes **MD5 for ETag** on every ordinary PutObject/UploadPart.

---

## PUT: `--s3fastput` implementation

### 1. Unsigned payload (`PayloadSigningPolicy::Never`)

Pass policy **2** into the `S3Client` / `S3CrtClient` constructor (elbencho: `source/toolkits/S3Tk.cpp`):

```cpp
std::make_shared<S3Client>(
    credentialsProvider,
    config,
    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,  // 2
    useVirtualAddressing);
```

Enum:

| Value | Policy | Meaning |
|------:|--------|---------|
| 0 | `RequestDependent` | SDK default: typically UNSIGNED-PAYLOAD on HTTPS, **signed body on HTTP** |
| 1 | `Always` | Always SHA-256 the body |
| 2 | `Never` | Always `x-amz-content-sha256=UNSIGNED-PAYLOAD` |

**You almost certainly need an AWS SDK patch** if the endpoint is **HTTP** (local RGW) or if you are on SDK ≥ ~1.11.4xx.

Upstream bugs / behavior:

- [aws-sdk-cpp#3297](https://github.com/aws/aws-sdk-cpp/issues/3297): `Never` is ignored after the default-checksum / flexible-checksum changes; the signer still hashes the body.
- Historical: non-HTTPS **forced** payload signing even when policy was `Never`.

elbencho’s patch: `external/patches/aws-sdk-cpp-unsigned-payload-never.patch`

What it changes in `AWSAuthV4Signer.cpp`:

1. **Classic signer** (`SignRequestWithCreds`): if policy is `Never`, set `payloadHash = UNSIGNED_PAYLOAD` and **do not** call `ComputePayloadHash`.
2. **SigV4a / CRT signer** (`SignRequestWithSigV4a`): do **not** force hashing just because the URI scheme is HTTP.

Without (1), `--s3fastput` is a no-op on current SDK. Without (2), HTTP-to-RGW still hashes.

Verify after porting (SDK debug log or `tcpdump`/`mitm`):

```
x-amz-content-sha256: UNSIGNED-PAYLOAD
```

If you still see a 64-hex SHA-256, the policy is not taking effect.

### 2. Disable request compression

On `ClientConfiguration` **before** constructing the client (elbencho: `S3Tk::initS3Client`):

```cpp
config.requestCompressionConfig.useRequestCompression =
    Aws::Client::UseRequestCompression::DISABLE;
```

`--s3fastput` sets this together with `Never`. You can also expose it separately.

### 3. Request checksum policy (do this even without a fastput flag)

**Before** `Aws::InitAPI()`:

```cpp
setenv("AWS_REQUEST_CHECKSUM_CALCULATION", "when_required", 0);  // 0 = do not overwrite user env
```

`overwrite=0` lets the operator force `when_supported` if they want chunked / trailer checksums.

`when_supported` (SDK default on recent versions) will:

- compute CRC32/CRC32C/… on every PUT
- often switch MPU to **chunked transfer** + streaming signatures (more CPU)

Alternative if the SDK version exposes it on config (1.11.4xx+):

```cpp
config.requestChecksumCalculation =
    Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;
```

Env var is more portable across SDK versions. It **must** be set before `InitAPI()`.

---

## GET: `--s3fastget` implementation

Two independent pieces. Do both.

### 1. Do not copy the body into the app

elbencho sets `GetObjectRequest::SetResponseStreamFactory` to a `/dev/null` `Aws::FStream` instead of a memory buffer (`LocalWorker::s3ModeDownloadObject`):

```cpp
request.SetResponseStreamFactory([]() {
    return new Aws::FStream("/dev/null", std::ios_base::out | std::ios_base::binary);
});
```

Without `--s3fastget`, the factory returns an `S3MemoryStream` wrapping the worker I/O buffer (needed for integrity check / GPU copy).

Also skip allocating that I/O buffer on GET-only runs (`LocalWorker::allocIOBuffer`).

Other apps: any discard sink is fine (`NullStream`, write-only streambuf that drops `xsputn`, etc.). Do **not** use a paced/limited streambuf that only implements `xsgetn` (read) as the GET sink — that caused RGW `206` + `callback failed` in this fork.

Incompatible with: data verification, GPU staging, any post-processing of payload bytes.

### 2. Skip SDK response-body checksum hashing

**Before** `Aws::InitAPI()`, and **only** when the fast-GET path is on:

```cpp
if (useFastGet)
    setenv("AWS_RESPONSE_CHECKSUM_VALIDATION", "when_required", 0);
```

| Value | SDK behavior |
|-------|----------------|
| `when_supported` (SDK default) | If the object/response has `x-amz-checksum-*`, hash the **entire GET body** and compare |
| `when_required` | Only validate if the request asked for checksums (`ChecksumMode::ENABLED`). Default GetObject does not, so hashing is skipped |

elbencho **does not** set `when_required` unless `--s3fastget` is on, so a normal GET still validates when the server sends checksum headers.

Config equivalent (newer SDK):

```cpp
config.responseChecksumValidation =
    Aws::Client::ResponseChecksumValidation::WHEN_REQUIRED;
```

Again: env must be set **before** `InitAPI()`.

---

## Init order (easy to get wrong)

```
1. Parse flags
2. setenv(AWS_EC2_METADATA_DISABLED)          // optional, avoids IMDS delay
3. setenv(AWS_REQUEST_CHECKSUM_CALCULATION)   // always when_required unless user overrode
4. setenv(AWS_RESPONSE_CHECKSUM_VALIDATION)   // only if fast-GET
5. Aws::InitAPI(options)                      // reads those env vars
6. Build ClientConfiguration
     - requestCompressionConfig
     - (optional) requestChecksumCalculation / responseChecksumValidation on config
7. new S3Client(..., PayloadSigningPolicy::Never, ...)
```

`Aws::InitAPI` / `curl_global_init` must run **before worker threads**. `ShutdownAPI` does not reset the “already initialized” flag — init once per process.

---

## What this will **not** fix

| Symptom | Cause | Where to fix |
|---------|--------|----------------|
| High CPU in `MD5_Update` / `EVP_DigestUpdate` **on the RGW host** during PUT | RGW ETag MD5 in `RGWPutObj::execute()` | Server: `rgw_debug_skip_put_md5` (benchmark-only) |
| High CPU in SHA-256 on the **client** despite `Never` | SDK ignoring `Never` | Apply the signer patch; confirm `UNSIGNED-PAYLOAD` on the wire |
| GET still hashes on the client with `when_required` | App set `ChecksumMode::ENABLED` or env overwritten | Don’t enable checksum mode on the request |
| MPU Complete still “slow” | Composite ETag is MD5 of 16-byte part etags — cheap | Ignore |

---

## Porting checklist (other app)

1. **PUT CPU**
   - [ ] `PayloadSigningPolicy::Never` (or language equivalent: unsigned payload).
   - [ ] Confirm `x-amz-content-sha256: UNSIGNED-PAYLOAD` on HTTP **and** HTTPS.
   - [ ] If not, patch `AWSAuthV4Signer` (or equivalent) — do not assume upstream honors `Never`.
   - [ ] `AWS_REQUEST_CHECKSUM_CALCULATION=when_required` before SDK init.
   - [ ] Disable request compression.
   - [ ] Do **not** set `x-amz-sdk-checksum-algorithm` / `--s3chksumalgo` unless you want that CPU back.

2. **GET CPU**
   - [ ] Discard the body (`/dev/null` or null streambuf); skip app-side copies.
   - [ ] `AWS_RESPONSE_CHECKSUM_VALIDATION=when_required` before SDK init.
   - [ ] Do not set `ChecksumMode::ENABLED` on GetObject unless you need it.

3. **Safety**
   - [ ] Fast-GET: disable integrity verification / GPU paths.
   - [ ] Unsigned payload over HTTP is weaker than signed payload; fine for local S3-compatible benches, not a substitute for TLS to the public internet.

4. **Language map (if not C++)**

   | Stack | Unsigned payload | Request checksums | Response checksums |
   |-------|------------------|-------------------|--------------------|
   | AWS SDK C++ | `PayloadSigningPolicy::Never` + likely signer patch | `AWS_REQUEST_CHECKSUM_CALCULATION` | `AWS_RESPONSE_CHECKSUM_VALIDATION` |
   | AWS CRT S3 (`aws-c-s3`) | `aws_s3_checksum_config` / unsigned payload in signing config | checksum config | checksum config |
   | boto3 / botocore | `config.s3['payload_signing_enabled'] = False` (HTTPS); HTTP may still sign | `request_checksum_calculation` | `response_checksum_validation` |
   | aws-sdk-go v2 | `s3.WithAPIOptions` / `v4.UnsignedPayloadMiddleware` | `RequestChecksumCalculation` | `ResponseChecksumValidation` |

---

## elbencho source map

| Piece | File |
|-------|------|
| Flag parse: `--s3fastput` → sign=Never + nocompress | `source/ProgArgs.cpp` (~1309) |
| `--s3unsigned` → sign=Never | `source/ProgArgs.cpp` (~1315) |
| Help text | `source/ProgArgs.cpp` (`--s3fastget`, `--s3fastput`, `--s3unsigned`, `--s3sign`) |
| Env vars before `InitAPI` | `source/toolkits/S3Tk.cpp` `initS3Global()` |
| Client: compression + `PayloadSigningPolicy` | `source/toolkits/S3Tk.cpp` `initS3Client()` |
| GET `/dev/null` stream factory | `source/workers/LocalWorker.cpp` `s3ModeDownloadObject` / `…Async` |
| Skip GET buffer alloc | `source/workers/LocalWorker.cpp` `allocIOBuffer()` |
| SDK `Never` patch | `external/patches/aws-sdk-cpp-unsigned-payload-never.patch` |
| Patch applied at SDK clone | `external/prepare-external.sh` `apply_awssdk_patches()` |
| SDK tag | `AWS_REQUIRED_TAG` default `1.11.789` |

CLI names: `--s3fastput`, `--s3fastget`, `--s3unsigned`, `--s3sign`, `--s3nocompress`, `--s3chksumalgo`.
