# Local Dump mirror (SLEEP/NET follow-up milestone)

The milestone brief points `fly -deps`/`fly -dump` at:

  https://github.com/thefly-lang/dump
  https://raw.githubusercontent.com/thefly-lang/dump/refs/heads/main/public/

## How `fly -deps` works now

`fly -deps` (see `src/fly/fly.cpp`) has two download strategies:

1. **HTTPS from Dump** (default): Uses `curl` to download `<name>.fly` from
   `https://raw.githubusercontent.com/thefly-lang/dump/refs/heads/main/public/`
   directly into the project's `module/` directory.

2. **Local mirror fallback**: If the HTTPS download fails (no network,
   curl not installed, 404, etc.), falls back to copying from this
   `dump-mirror/` directory. Also used when `fly -deps --offline` is
   specified.

## Packages mirrored here (local fallback)

  sleep.fly   -- the SLEEP v1.0 library (public Fly module)
  http.fly    -- HTTP/1.1 client built on `bring net` (public Fly module)

## Offline mode

Use `fly -deps --offline` to skip the HTTPS download entirely and install
only from this local mirror. Useful for air-gapped environments or when
you want to test against the exact local copies.
