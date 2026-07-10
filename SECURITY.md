# Security policy

## Supported versions

posthog-c is an unofficial pre-1.0 project. Security fixes are applied to the
latest commit on `main`; older 0.x snapshots are not maintained as separate
release lines.

## Reporting a vulnerability

Please use GitHub's **Security > Report a vulnerability** flow so the report is
private. Do not include secrets, project API keys, event payloads, or a working
exploit in a public issue. Include the affected commit, platform/toolchain,
configuration, impact, and the smallest safe reproducer you can provide.

Expect an acknowledgement within seven days. Since this is currently a
maintainer-run preview, no formal SLA or bug-bounty program is offered.

## Security boundaries

- PostHog project keys are write-only ingestion tokens, but should still be
  treated as sensitive configuration.
- `offline_path` stores the token and event properties as plaintext. The SDK
  creates private files on POSIX and inherits the directory ACL on Windows; the
  host remains responsible for protecting that directory.
- `crash_handler` is opt-in, process-global, and best-effort. In-process stack
  walking is not safe against every form of heap or loader corruption.
- HTTPS is implemented with the Windows trust store. Linux/macOS HTTPS remains
  unsupported until the documented TLS backends land; use a trusted local
  plaintext proxy only in an environment where that is appropriate.
