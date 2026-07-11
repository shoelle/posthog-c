#!/usr/bin/env bash
# Load .env (gitignored) and run the opt-in live PostHog contract test.
# Requires a .env with POSTHOG_API_KEY (copy from .env.example).
# HTTPS works on all three desktops: WinHTTP (Windows), Secure Transport (macOS),
# OpenSSL (Linux - needs libssl-dev). POSTHOG_HOST may also be plaintext http://.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -f .env ]; then
  echo "No .env found. Copy .env.example to .env and set POSTHOG_API_KEY." >&2
  exit 1
fi
set -a
. ./.env
set +a
zig build -Doptimize=ReleaseSafe live-contract
