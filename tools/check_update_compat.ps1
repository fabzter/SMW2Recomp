<#
.SYNOPSIS
  Release gate: an update must keep the player's in-game saves and save states.

.DESCRIPTION
  Reproduces what a player on the previous release goes through:

    1. Builds the previous release (a clean clone of -PreviousTag with its
       framework patches and this checkout's generated code).
    2. Starts it, writes a save state through the real save-state menu and
       lets the game write its in-game save (save.srm).
    3. Loads that state in the previous build and records a WRAM trace.
    4. Installs the new setup pack with the previous release's own updater
       script (the one players actually run), then rebuilds like
       "Generate & rebuild".
    5. Checks that every file in build\saves is byte-identical, then loads the
       old state in the new build and records the same trace.

  FAIL when a save file changed, the old state does not load in the new build,
  or the new build, after loading it, does not run exactly like the previous
  build does from the same state (WRAM trace). -AllowLogicChange turns that
  last one into a warning for a release that changes game logic on purpose.

  Needs this checkout's generated code (src/gen, recomp/funcs.h) and rom.cfg,
  so run it after a local build. Nothing in the checkout is modified.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\check_update_compat.ps1 `
    -PreviousTag v0.1.2 -Pack dist\donkeykongcountry2-0.1.3-windows-x64.zip
#>
param(
  [Parameter(Mandatory = $true)][string]$PreviousTag,
  [Parameter(Mandatory = $true)][string]$Pack,
  [string]$WorkDir = (Join-Path $env:TEMP 'dkc2-update-compat'),
  [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat',
  [string]$ToolsPath = 'C:\dkcr\toolsenv\Scripts',
  [int]$SaveFrame = 1500,
  [int]$LoadFrame = 200,
  [int]$RunFrames = 900,
  # Keep a copy of the previous release's game in <WorkDir>\previous_build
  # (and keep WorkDir on success) for before/after picture comparisons.
  [switch]$KeepPreviousBuild,
  # A release that changes game logic on purpose: after the load the builds
  # may diverge (reported as a warning instead of a failure).
  [switch]$AllowLogicChange
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# The game target is the build's one *SNESRecomp.exe; the updater is the
# title's src\*_update.c.
$exeName = @(Get-ChildItem (Join-Path $repo 'build') -Filter '*SNESRecomp.exe' -ErrorAction SilentlyContinue)[0].Name
$updaterSource = @(Get-ChildItem (Join-Path $repo 'src') -Filter '*_update.c')[0].Name
if (-not $exeName -or -not $updaterSource) { throw 'cannot find build\*SNESRecomp.exe or src\*_update.c' }
$Pack = (Resolve-Path $Pack).Path
foreach ($p in 'src\gen', 'recomp\funcs.h') {
  if (-not (Test-Path (Join-Path $repo $p))) { throw "missing ${p}: build this checkout first" }
}
# The setup's rom.cfg can name a ROM that has since moved; the game's own
# build\rom.cfg is then the one that works.
$rom = $null
foreach ($cfgPath in (Join-Path $repo 'rom.cfg'), (Join-Path $repo 'build\rom.cfg')) {
  if (-not (Test-Path $cfgPath)) { continue }
  $candidate = (Get-Content $cfgPath -Raw).Trim()
  if ($candidate -and (Test-Path -LiteralPath $candidate)) { $rom = $candidate; break }
}
if (-not $rom) { throw 'no rom.cfg names a ROM that exists: build and run this checkout first' }
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory $WorkDir | Out-Null
$install = Join-Path $WorkDir 'install'
$build = Join-Path $install 'build'
$saves = Join-Path $build 'saves'
$builds = @()

# Windows PowerShell turns a native program's stderr into errors; only the
# exit code says whether it failed, so native calls run with 'Continue'.
function Invoke-Git {
  $ErrorActionPreference = 'Continue'
  $out = & git @args 2>&1
  if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 5; throw "git $($args -join ' ') failed" }
}

function Invoke-Build {
  $ErrorActionPreference = 'Continue'
  $bat = Join-Path $WorkDir 'build.bat'
  @"
@echo off
call "$VcVars" >nul || exit /b 1
set "PATH=$ToolsPath;%PATH%"
cd /d "$install" || exit /b 1
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build --parallel 6 || exit /b 1
"@ | Set-Content -Encoding ascii $bat
  $log = cmd /c "`"$bat`"" 2>&1
  if ($LASTEXITCODE -ne 0) { $log | Select-Object -Last 20; throw 'build failed' }
  $script:builds += @((Get-FileHash (Join-Path $build $exeName)).Hash)
}

function Invoke-Game([string]$Tag, [hashtable]$Vars) {
  foreach ($k in $Vars.Keys) { Set-Item "env:SNESRECOMP_$k" $Vars[$k] }
  try {
    $p = Start-Process (Join-Path $build $exeName) -ArgumentList "`"$rom`"" -WorkingDirectory $build `
      -PassThru -WindowStyle Minimized -RedirectStandardError (Join-Path $WorkDir "$Tag.err")
    if (-not $p.WaitForExit(300000)) { $p.Kill(); throw "$Tag timed out" }
  } finally {
    foreach ($k in $Vars.Keys) { Remove-Item "env:SNESRECOMP_$k" }
  }
}

function Get-SaveHashes {
  @(Get-ChildItem $saves -Recurse -File | Sort-Object FullName | ForEach-Object {
      $_.FullName.Substring($saves.Length) + ' ' + (Get-FileHash $_.FullName).Hash })
}

# The updater the previous release runs, without starting the setup program.
function Get-OldUpdater {
  $c = Get-Content (Join-Path $install "src\$updaterSource") -Raw
  # A string literal cannot hold a raw newline, so the first '";' that ends a
  # line ends the initializer.
  $body = [regex]::Match($c, '(?s)kUpdaterScript\[\] =(.*?)"\s*;[ \t]*\r?\n').Value
  $unescape = [System.Text.RegularExpressions.MatchEvaluator] {
    param($e) switch ($e.Groups[1].Value) { 'r' { "`r" } 'n' { "`n" } default { $e.Groups[1].Value } } }
  $sb = New-Object System.Text.StringBuilder
  foreach ($m in [regex]::Matches($body, '"((?:[^"\\]|\\.)*)"')) {
    [void]$sb.Append([regex]::Replace($m.Groups[1].Value, '\\(.)', $unescape))
  }
  $sb.ToString().Replace('Start-Process -FilePath $setup -WorkingDirectory $Root', "Write-Host 'setup would start'").
    Replace('Start-Sleep -Seconds 4', '').Replace("Read-Host 'Press Enter to close'", '')
}

# Did the load take effect? Best case the run continues the saved one exactly
# (frame after the load = a save-run frame). Some titles keep a little state
# outside the snapshot and do not continue bit-exactly even in the build that
# saved it; the load still took effect when the run leaves the unloaded
# timeline (the save run, identical to it up to $SaveFrame).
function Get-LoadResult([string]$TracePath, [hashtable]$SaveRun, [string[]]$SaveLines) {
  $after = @(Get-Content $TracePath | Select-Object -Skip $LoadFrame -First 5)
  foreach ($line in $after) {
    $f = $line.Split(',')
    if ($SaveRun.ContainsKey($f[1])) { return "continues the saved run (frame $($f[0]) = save-run frame $($SaveRun[$f[1]]))" }
  }
  for ($i = 0; $i -lt $after.Count; $i++) {
    if ($after[$i] -ne $SaveLines[$LoadFrame + $i]) { return 'state loaded (the run left the unloaded timeline)' }
  }
  return $null
}

Write-Host "== Previous release $PreviousTag"
Invoke-Git -c core.longpaths=true clone -q --recurse-submodules --branch $PreviousTag $repo $install
Invoke-Git -C $install config core.longpaths true
foreach ($p in Get-ChildItem (Join-Path $install 'patches') -Filter *.patch | Sort-Object Name) {
  $sub = if ($p.Name -like 'recomp-ui-*') { 'recomp-ui' } else { 'snesrecomp' }
  Invoke-Git -C (Join-Path $install $sub) apply $p.FullName
}
Copy-Item -Recurse (Join-Path $repo 'src\gen') (Join-Path $install 'src\gen') -Force
Copy-Item (Join-Path $repo 'recomp\funcs.h') (Join-Path $install 'recomp\funcs.h') -Force
$rom | Set-Content -NoNewline -Encoding ascii (Join-Path $install 'rom.cfg')
Invoke-Build
New-Item -ItemType Directory -Force $saves | Out-Null
"[General]`r`nSkipLauncher = 1`r`n[Sound]`r`nEnableAudio = 0`r`n" | Set-Content -Encoding ascii (Join-Path $build 'config.ini')
$rom | Set-Content -NoNewline -Encoding ascii (Join-Path $build 'rom.cfg')

$saveTrace = Join-Path $WorkDir 'trace_save.txt'
Invoke-Game 'save' @{ RUN_FRAMES = "$($SaveFrame + 100)"; OVERLAY_SELFTEST = "$SaveFrame";
  OVERLAY_SELFTEST_SAVEONLY = '1'; STATE_TRACE = $saveTrace }
$states = @(Get-ChildItem $saves -Filter *.sav)
if (-not $states) { throw 'the previous build wrote no save state' }
$hasSram = (Select-String -Path (Join-Path $install 'src\main.c') -Pattern '\.sram_path\s*=\s*"' -Quiet)
if ($hasSram -and -not (Test-Path (Join-Path $saves 'save.srm'))) { throw 'the previous build wrote no save.srm' }
$saveRun = @{}
$saveLines = @(Get-Content $saveTrace)
$saveLines | ForEach-Object { $f = $_.Split(','); $saveRun[$f[1]] = $f[0] }
$oldTrace = Join-Path $WorkDir 'trace_old.txt'
Invoke-Game 'load_old' @{ RUN_FRAMES = "$RunFrames"; OVERLAY_SELFTEST_LOADAT = "$LoadFrame"; STATE_TRACE = $oldTrace }
$oldMatch = Get-LoadResult $oldTrace $saveRun $saveLines
if (-not $oldMatch) { throw 'the previous build cannot load its own state; the check itself is broken' }
$before = Get-SaveHashes
if ($KeepPreviousBuild) {
  & robocopy.exe $build (Join-Path $WorkDir 'previous_build') /E /XD saves CMakeFiles /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
}

Write-Host "== Update to $(Split-Path -Leaf $Pack)"
$updater = Join-Path $WorkDir 'updater.ps1'
Get-OldUpdater | Set-Content -Encoding utf8 $updater
$url = 'file:///' + $Pack.Replace('\', '/')
& {
  $ErrorActionPreference = 'Continue'
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -Root $install -Url $url -WaitPid 0 -Version check 2>&1 |
    Out-File -Encoding utf8 (Join-Path $WorkDir 'updater.log')
}
$installed = (Get-Content (Join-Path $install 'VERSION') -Raw).Trim()
if ("v$installed" -eq $PreviousTag) { throw "the updater did not install the pack (see $WorkDir\updater.log)" }
Write-Host "installed $installed"
Invoke-Build
$after = Get-SaveHashes
$newTrace = Join-Path $WorkDir 'trace_new.txt'
Invoke-Game 'load_new' @{ RUN_FRAMES = "$RunFrames"; OVERLAY_SELFTEST_LOADAT = "$LoadFrame"; STATE_TRACE = $newTrace }
$newMatch = Get-LoadResult $newTrace $saveRun $saveLines

$fail = @()
if ($builds[0] -eq $builds[-1]) { $fail += 'the rebuild after the update produced the same game executable' }
if (($before -join "`n") -ne ($after -join "`n")) {
  $fail += 'save files changed during the update:'
  $fail += Compare-Object $before $after | ForEach-Object { "  $($_.SideIndicator) $($_.InputObject)" }
}
if (-not $newMatch) { $fail += "the new build did not load the $PreviousTag save state" }
$old = Get-Content $oldTrace; $new = Get-Content $newTrace
$firstDiff = $null
for ($i = $LoadFrame; $i -lt [Math]::Min($old.Count, $new.Count); $i++) {
  if ($old[$i] -ne $new[$i]) { $firstDiff = $i + 1; break }
}

Write-Host ''
Write-Host "saves checked: $($before.Count) file(s), state(s): $(($states | ForEach-Object Name) -join ', ')$(if (-not $hasSram) { ' (this game has no in-game save)' })"
Write-Host "old build after load: $oldMatch"
Write-Host "new build after load: $newMatch"
if ($firstDiff -and $AllowLogicChange) {
  Write-Host "WARNING: after the load the new build diverges from the old one at frame $firstDiff." -ForegroundColor Yellow
  Write-Host '         Allowed by -AllowLogicChange; review that the game plays on correctly.' -ForegroundColor Yellow
} elseif ($firstDiff) {
  $fail += "after loading the old state the new build diverges from the old one at frame $firstDiff " +
    '(pass -AllowLogicChange only for a release that changes game logic on purpose)'
} else {
  Write-Host "after the load both builds are identical for $($RunFrames - $LoadFrame) frames"
}
if ($fail) {
  $fail | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
  Write-Host "evidence kept in $WorkDir"
  exit 1
}
if (-not $KeepPreviousBuild) { Remove-Item -Recurse -Force $WorkDir }
Write-Host 'PASS: saves and save states survive the update' -ForegroundColor Green
exit 0
