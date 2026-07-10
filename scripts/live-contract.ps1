#!/usr/bin/env pwsh
# Load .env (gitignored) and run the opt-in live PostHog contract test.
# Requires a .env with POSTHOG_API_KEY (copy from .env.example).
# Real HTTPS delivery is Windows-only today (WinHTTP); on Linux/macOS point
# POSTHOG_HOST at a plaintext http:// proxy until the TLS backends land.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".env"
if (-not (Test-Path $envFile)) {
    Write-Host "No .env found. Copy .env.example to .env and set POSTHOG_API_KEY." -ForegroundColor Yellow
    exit 1
}
foreach ($line in Get-Content $envFile) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith("#") -or -not $t.Contains("=")) { continue }
    $i = $t.IndexOf("=")
    $name = $t.Substring(0, $i).Trim()
    $value = $t.Substring($i + 1).Trim()
    [Environment]::SetEnvironmentVariable($name, $value, "Process")
}
Push-Location $root
try { zig build -Doptimize=ReleaseSafe live-contract } finally { Pop-Location }
