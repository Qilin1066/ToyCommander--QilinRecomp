param([string]$Stub, [string]$Ninja, [string]$Launcher, [string]$Out, [string]$Game, [string]$Qilin)
$ErrorActionPreference = 'Stop'
$exeName = $Game + '.exe'
$archiveName = 'lib' + $Game.ToLowerInvariant() + '_kit.a'
# SHA-256 of a file, upper-case hex. Not Get-FileHash: that cmdlet is missing
# when Windows PowerShell inherits a PowerShell 7 module path, as it does
# when started from pwsh (or from CI's shell).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path))  # .NET resolves against the process folder, not PowerShell location
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
# Test metacharacters and names outside the Windows active code page, too.
$fixture = Join-Path $Out ($Game + ' kit with spaces ' + [char]0x00e9)
if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit/lib'), (Join-Path $fixture 'kit/include'), (Join-Path $fixture 'kit/toolchain/bin'), (Join-Path $fixture 'work') -Force | Out-Null
$disc = Join-Path $fixture ('disc %PATH% & ' + [char]0x4fdd + [char]0x5b58 + '.cue')
Set-Content -LiteralPath $disc -Value 'synthetic cue, no game data'
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $fixture 'kit/play.ps1')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/qilin.exe')
# A Chocolatey ninja.exe is a shim that finds the real one by a relative
# path, so a copy of it doesn't run: copy what it points to.
$chocolateyNinja = Join-Path (Split-Path -Parent (Split-Path -Parent $Ninja)) 'lib/ninja/tools/ninja.exe'
if (Test-Path -LiteralPath $chocolateyNinja) { $Ninja = $chocolateyNinja }
Copy-Item -LiteralPath $Ninja -Destination (Join-Path $fixture 'kit/ninja.exe')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/toolchain/bin/x86_64-w64-mingw32-clang++.exe')
Set-Content -LiteralPath (Join-Path $fixture 'kit/include/stub.hpp') -Value 'header1'
Set-Content -LiteralPath (Join-Path $fixture 'kit/VERSION') -Value 'launcher-test'
Set-Content -LiteralPath (Join-Path $fixture 'kit/game.txt') -Value "title=$Game`nexe=$exeName`nboot=fixture.bin`nbuild=fixture`nrelease=fixture"
Set-Content -LiteralPath (Join-Path $fixture ('kit/lib/' + $archiveName)) -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fixture 'kit/kit.ninja') -Value "cxxflags =`nldflags =`nlibs ="
Set-Content -LiteralPath (Join-Path $fixture 'work/disc.txt') -Value $disc -Encoding UTF8
$shellExe = (Get-Process -Id $PID).Path
$launcherPath = Join-Path $fixture 'kit/play.ps1'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0) { throw 'Initial build failed' }
$stamp = Join-Path $fixture 'work/built-with.txt'
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture $exeName
$initialHash = (Get-Sha256 $game)
$calls = Join-Path $fixture 'work/tool-calls.txt'
function Compile-Count { return @(Get-Content -LiteralPath $calls | Where-Object { $_ -eq 'compile' }).Count }
$compiles = Compile-Count
if ($compiles -ne 1) { throw 'Initial Ninja build did not compile exactly once' }
$archive = Join-Path $fixture ('kit/lib/' + $archiveName)
Add-Content -LiteralPath $archive -Value 'same version, new contents'
(Get-Item -LiteralPath $archive).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Get-Content -LiteralPath $stamp -Raw) -eq $before) { throw 'Same-version change did not rebuild' }
if ((Get-Sha256 $game) -eq $initialHash) { throw 'Old archive timestamp hid a changed runtime: stale link' }
if ((Compile-Count) -ne $compiles) { throw 'Runtime-only update unnecessarily recompiled guest code' }
# Header and compiler changes must rebuild objects even with old timestamps.
$header = Join-Path $fixture 'kit/include/stub.hpp'
Set-Content -LiteralPath $header -Value 'header2'
(Get-Item -LiteralPath $header).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Changed header with old timestamp did not compile' }
$compiles = Compile-Count
$compiler = Join-Path $fixture 'kit/toolchain/bin/x86_64-w64-mingw32-clang++.exe'
Add-Content -LiteralPath $compiler -Value 'compiler-overlay'
(Get-Item -LiteralPath $compiler).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Changed compiler with old timestamp did not compile' }
$compiles = Compile-Count
# -march=native must be recompiled after moving the kit to another CPU.
$oldCpu = $env:PROCESSOR_IDENTIFIER
try {
    $env:PROCESSOR_IDENTIFIER = 'synthetic different processor'
    & $shellExe -NoProfile -File $launcherPath -BuildOnly
    if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'CPU change did not recompile' }
    $compiles = Compile-Count
    $callCount = @(Get-Content -LiteralPath $calls).Count
    & $shellExe -NoProfile -File $launcherPath -BuildOnly
    if ($LASTEXITCODE -ne 0 -or @(Get-Content -LiteralPath $calls).Count -ne $callCount) { throw 'Unchanged kit was rebuilt' }
    & $shellExe -NoProfile -File $launcherPath -BuildOnly -Rebuild
    if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Explicit rebuild did not compile' }
} finally { $env:PROCESSOR_IDENTIFIER = $oldCpu }
if (-not (Test-Path -LiteralPath (Join-Path $fixture ($exeName + '.previous')))) { throw 'Previous executable missing' }
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture $exeName
$hash = (Get-Sha256 $game)
Add-Content -LiteralPath (Join-Path $fixture ('kit/lib/' + $archiveName)) -Value 'another update'
$locked = [System.IO.File]::Open($game, 'Open', 'Read', 'Read')
try {
    & $shellExe -NoProfile -File $launcherPath -BuildOnly
    if ($LASTEXITCODE -eq 0) { throw 'Locked executable unexpectedly replaced' }
} finally { $locked.Dispose() }
if ((Get-Sha256 $game) -ne $hash -or (Get-Content -LiteralPath $stamp -Raw) -ne $before) { throw 'Failed update changed executable or stamp' }
# Recover and launch a real child with Unicode, spaces and shell metacharacters.
& $shellExe -NoProfile -File $launcherPath
if ($LASTEXITCODE -ne 0) { throw 'Launch failed' }
$log = Join-Path $fixture 'launched.txt'
for ($attempt = 0; $attempt -lt 50; ++$attempt) {
    if ((Test-Path -LiteralPath $log) -and (Get-Content -LiteralPath $log -Raw -Encoding UTF8) -eq $disc) { break }
    Start-Sleep -Milliseconds 100
}
if ((Get-Content -LiteralPath $log -Raw -Encoding UTF8).Trim() -ne $disc) { throw 'Child arguments were damaged' }
Write-Host 'Real Ninja: old timestamps, headers/compiler/CPU changes, explicit rebuild, locked installation and Unicode arguments passed'

# The real recompiler, not the fixture: Unicode command line and output path.
$boot = Join-Path $fixture ([char]0x4fdd + [char]0x5b58 + '.bin')
$report = Join-Path $fixture ([char]0x4fdd + [char]0x5b58 + '.json')
[IO.File]::WriteAllBytes($boot, [byte[]](9, 0))
& $Qilin info $boot --json $report
if ($LASTEXITCODE -ne 3) { throw 'Synthetic unsupported boot did not return its expected status' }
$info = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if ($info.sha256.Length -ne 64) { throw 'The real recompiler lost its Unicode input or output path' }
