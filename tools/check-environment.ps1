param(
    [string]$BundlesRoot = (Join-Path $env:LOCALAPPDATA 'stm32cube\bundles')
)

$ErrorActionPreference = 'Stop'

# Resolve full paths explicitly rather than using whatever happens to be in PATH.
$compilerRoot = Join-Path $BundlesRoot 'gnu-tools-for-stm32\14.3.1+st.2'
$manifestPath = Join-Path $compilerRoot 'CubeBundle.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing ST toolchain manifest: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.name -ne 'gnu-tools-for-stm32' -or $manifest.version -ne '14.3.1+st.2') {
    throw 'Expected ST gnu-tools-for-stm32 14.3.1+st.2.'
}

$checks = @(
    @{ Name = 'GCC'; Path = (Join-Path $compilerRoot 'bin\arm-none-eabi-gcc.exe'); Pattern = '\b14\.3\.1\b' },
    @{ Name = 'CMake'; Path = (Join-Path $BundlesRoot 'cmake\4.4.0+st.1\bin\cmake.exe'); Pattern = '^cmake version 4\.4\.0\s*$' },
    @{ Name = 'Ninja'; Path = (Join-Path $BundlesRoot 'ninja\1.13.2+st.1\bin\ninja.exe'); Pattern = '^1\.13\.2\s*$' }
)

foreach ($check in $checks) {
    if (-not (Test-Path -LiteralPath $check.Path -PathType Leaf)) {
        throw "Missing $($check.Name): $($check.Path)"
    }
    Write-Host "$($check.Name): $($check.Path)"
    $versionOutput = @(& $check.Path --version)
    if ($LASTEXITCODE -ne 0) {
        throw "$($check.Name) version query failed (exit $LASTEXITCODE)."
    }
    $firstLine = $versionOutput | Select-Object -First 1
    if ($firstLine -notmatch $check.Pattern) {
        throw "Unexpected $($check.Name) version: $firstLine"
    }
    Write-Host "  $firstLine"
}

Write-Host 'Environment version checks passed. Firmware build and hardware checks remain separate.'
