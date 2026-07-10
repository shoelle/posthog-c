# Changelog

All notable changes to this project will be documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this unofficial 0.x
project does not promise source or binary compatibility between releases.

## [Unreleased]

No changes yet.

## [0.1.0] - 2026-07-10

Initial public preview: C/C++ capture, native delivery and offline spill,
posthog-js WASM bridge, feature flags, error tracking, and opt-in crash capture.

### Added

- Stable host-owned anonymous identity contract and `ph_get_distinct_id()`.
- Bounded `/flags/` response framing tests and HTTP parser fuzz coverage.
- ReleaseSafe, sanitizer, fuzz, WASM, and downstream package CI coverage.
- An opt-in real-service contract workflow for ingestion and feature flags.
- Security policy and contribution guidance.

### Changed

- Feature flag caches are scoped to identity/group evaluation context.
- Public property capacities are a fixed source/ABI contract.
- Native event clocks include suspend and UUID/reset entropy comes from the OS.
- Documentation now states the native/WASM semantic boundary explicitly.
- Transport operations share a whole-request deadline.

### Fixed

- Locale-dependent JSON numbers and acceptance of malformed JSON/UTF-8.
- Sender startup failures, callback teardown deadlocks, and control-event
  privacy bypasses.
- Offline spill durability, permissions, replay accounting, and loss reporting.
- Crash-record replacement, premature deletion, handler-state restoration, and
  overbroad async-signal-safety claims.
