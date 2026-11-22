# Play the game (the Windows download's "Play <game>.bat" runs this). The
# same script serves both games' downloads: kit\game.txt names the game.
#
# The download holds everything but the game's own code: that is made here,
# from your own disc dump, the first time (and again after an update): qilin
# lifts the disc's boot file into C++, the bundled compiler (LLVM-MinGW)
# compiles it, and it's linked with the prebuilt rest into the game's .exe.
# Then, and every time after, it starts the game with the disc dump.
#
#   play.ps1 [-Rebuild] [-BuildOnly] [-ChooseDisc]
#
# -Rebuild builds again even if the .exe is up to date; -BuildOnly doesn't
# start the game afterwards; -ChooseDisc asks for the disc's .cue again.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [switch]$ChooseDisc
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}

$kit = $PSScriptRoot
$top = Split-Path -Parent $kit
$work = Join-Path $top "work"
$about = @{}
foreach ($line in Get-Content (Join-Path $kit "game.txt")) {
    $at = $line.IndexOf("=")
    if ($at -gt 0) { $about[$line.Substring(0, $at)] = $line.Substring($at + 1).Trim() }
}
$title = $about["title"]
$exeName = $about["exe"]
$game = Join-Path $top $exeName
$stamp = Join-Path $work "built-with.txt"
$discFile = Join-Path $work "disc.txt"
$kitVersion = (Get-Content (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# The game is compiled for this PC's own processor (-march=native), so a
# build is for one CPU: it's built again if the folder moves to a PC with a
# different one.
$cpu = if ($onWindows) { "$env:PROCESSOR_IDENTIFIER" } else { "cross" }
# Streamed SHA-256 works in Windows PowerShell even when its module search
# path was inherited from PowerShell 7.
function Get-Sha256([string]$path) {
    $stream = [IO.File]::OpenRead($path)
    $sha = $null
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
    } finally {
        if ($sha) { $sha.Dispose() }
        $stream.Dispose()
    }
}
function Get-Identity([string[]]$parts) {
    $lines = foreach ($part in $parts) { $part.Replace('\', '/') + ':' + (Get-Sha256 (Join-Path $kit $part)) }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))).Replace('-', '') }
    finally { $sha.Dispose() }
}
$libraryName = 'lib' + [IO.Path]::GetFileNameWithoutExtension($exeName).ToLowerInvariant() + '_kit.a'
$headers = @(Get-ChildItem -LiteralPath (Join-Path $kit 'include') -Recurse -File | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($kit.Length + 1) })
$compileKit = Get-Identity (@('kit.ninja', 'toolchain/bin/x86_64-w64-mingw32-clang++.exe') + $headers)
$kitIdentity = Get-Identity @("lib/$libraryName", 'qilin.exe', 'game.txt', 'play.ps1')
$builtFor = "$kitVersion | $cpu | $kitIdentity | $compileKit"
# CPU identity must affect Ninja's command, not just the outer build stamp:
# otherwise -march=native objects from a different PC may be reused.
$sha = [Security.Cryptography.SHA256]::Create()
try { $compileRevision = 'rev' + ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes("$cpu|$compileKit")))).Replace('-', '') }
finally { $sha.Dispose() }
# Off Windows (testing the download under Wine), QILIN_KIT_RUNNER runs the kit's programs.
$runner = $env:QILIN_KIT_RUNNER

# Runs one of the kit's programs, its output to the window; returns its exit code.
function Invoke-Kit([string]$exe, [string[]]$arguments) {
    if ($runner) { & $runner $exe @arguments | Out-Host } else { & $exe @arguments | Out-Host }
    return $LASTEXITCODE
}
# The same, its output (error output too) returned as text, with the exit code.
function Read-Kit([string]$exe, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns redirected error output into errors
    $text = if ($runner) { & $runner $exe @arguments 2>&1 | Out-String } else { & $exe @arguments 2>&1 | Out-String }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    return @{ text = $text; code = $code }
}

$qilin = Join-Path $kit "qilin.exe"

# Whether a file is a dump of the disc this port supports. Returns "" if it
# is, else why not.
function Test-Disc([string]$cue) {
    if (-not (Test-Path -LiteralPath $cue -PathType Leaf)) { return "there's no such file" }
    if ([IO.Path]::GetExtension($cue).ToLower() -ne ".cue") { return "it isn't a .cue file (choose the dump's .cue, beside its .bin tracks)" }
    $report = Read-Kit $qilin @("info", $cue)
    if ($report.code -ne 0) { return "qilin can't read it: $($report.text.Trim())" }
    $info = $report.text | ConvertFrom-Json
    if (-not $info.build) { return "its boot file isn't the supported build (SHA-256 $($info.sha256)): this port needs $($about['release'])" }
    if ($info.build.title -ne $about["build"]) { return "it's $($info.build.title): this download is for $($about['build'])" }
    return ""
}

# The disc: the one used last, else ask (until one is the supported build).
$disc = if (-not $ChooseDisc -and (Test-Path $discFile)) { (Get-Content $discFile -Raw).Trim() } else { "" }
while ($true) {
    if ($disc) {
        $why = Test-Disc $disc
        if (-not $why) { break }
        Write-Host "$disc can't be used: $why" -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "no disc: write the .cue's path into $discFile" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your $title disc dump: its .cue file (with the .bin tracks beside it)"
    $dialog.Filter = "Disc dump (*.cue)|*.cue"
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "No disc chosen."
        exit 0
    }
    $disc = $dialog.FileName
}
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Start-Game {
    if (-not $onWindows) { Write-Host '(not on Windows: built, not started)'; return }
    Start-Process -FilePath $game -ArgumentList "`"$disc`"" -WorkingDirectory $top -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $top 'run.log') -RedirectStandardError (Join-Path $top 'run-errors.log')
}

# Up to date: just play.
$built = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path $game) -and $built -eq $builtFor) {
    Set-Content -LiteralPath $discFile -Value $disc -Encoding UTF8
    if (-not $BuildOnly) { Start-Game }
    exit 0
}

Write-Host "$title QilinRecomp $kitVersion" -ForegroundColor Cyan
if ((Test-Path $game) -and $built -and -not $built.StartsWith("$kitVersion |")) { Write-Host "Updating the game for this version: a few minutes." }
elseif (Test-Path $game) { Write-Host "Building the game for this PC's processor: a few minutes." }
else { Write-Host "Building the game from your disc. This happens once (and after an update): a few minutes (up to 15 on a slower PC)." }
Write-Host "Disc: $disc"
Write-Host ""

# Lift the boot file (files whose text is unchanged are left alone, so an
# update recompiles only what changed).
Write-Host "== Recompiling $($about['boot']) into C++" -ForegroundColor Cyan
$gen = Join-Path $work "gen"
if ((Invoke-Kit $qilin @("lift", $disc, "--out", $gen, "--files", "24")) -ne 0) { throw "qilin lift failed" }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem $gen -Filter *.cpp | Sort-Object Name)
$kitLibrary = "../kit/lib/lib" + [IO.Path]::GetFileNameWithoutExtension($exeName).ToLower() + "_kit.a"
$lines = @(
    "# Written by play.ps1: the game's lifted code, compiled against the kit.",
    "kit = ../kit",
    "include ../kit/kit.ninja",
    "cxx = ..\kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
    "native = -march=native",
    "rule cxx",
    "  command = `$cxx `$cxxflags `$native -DQILIN_KIT_COMPILE_ID=$compileRevision -Igen -MD -MF `$out.d -c `$in -o `$out",
    "  depfile = `$out.d",
    "  deps = gcc",
    "  description = Compiling `$in",
    "rule link",
    "  command = `$cxx `$ldflags -o `$out `$in `$libs",
    "  description = Linking `$out"
)
if (-not $onWindows) { $lines[3] = "cxx = " + $env:QILIN_KIT_CXX; $lines[4] = "native =" }   # testing off Windows: a cross compiler, for any CPU
if (-not $sources.Count) { throw "qilin lift produced no C++ sources" }
$objects = @()
foreach ($s in $sources) {
    $object = "obj/$($s.BaseName).o"
    $objects += $object
    $lines += "build ${object}: cxx gen/$($s.Name)"
}
$lines += "build force_link: phony"
$lines += "build ${exeName}: link $($objects -join ' ') | $kitLibrary force_link"
$lines += "default $exeName"
Set-Content -Path (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

Write-Host "== Compiling ($($sources.Count) files; the biggest take a few minutes each)" -ForegroundColor Cyan
$ninja = if ($onWindows) { Join-Path $kit "ninja.exe" } else { "ninja" }
if ($Rebuild) {
    if ($onWindows) { $clean = Invoke-Kit $ninja @('-C', $work, '-t', 'clean') }
    else { & $ninja -C $work -t clean | Out-Host; $clean = $LASTEXITCODE }
    if ($clean -ne 0) { throw "cleaning the generated outputs failed" }
}
$jobs = [Math]::Max(1, [Environment]::ProcessorCount)
if ($onWindows) { $code = Invoke-Kit $ninja @("-C", $work, "-j", "$jobs") }
else { & $ninja -C $work -j $jobs | Out-Host; $code = $LASTEXITCODE }
if ($code -ne 0) { throw "compiling failed" }
# Stage beside the destination; a failed copy leaves the old executable intact.
$staged = "$game.new"
Copy-Item -LiteralPath (Join-Path $work $exeName) -Destination $staged -Force
if (Test-Path -LiteralPath $game -PathType Leaf) {
    [System.IO.File]::Replace($staged, $game, "$game.previous", $true)
} else {
    [System.IO.File]::Move($staged, $game)
}
Set-Content -LiteralPath "$discFile.tmp" -Value $disc -Encoding UTF8
Move-Item -LiteralPath "$discFile.tmp" -Destination $discFile -Force
Set-Content -Path $stamp -Value $builtFor -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting $title..."
    Start-Game
}
exit 0
