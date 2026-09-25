# Raster candidate captures for R4 (docs/lumen_lite_design.md, R4.1).
#
# Foreground, Release, Cornell and the living room only.  No path tracing: R4
# reuses the R3 references and this script never sets MIRABILIS_TEST_TRACE.
# Every run starts from a cleared environment and its variables are written
# next to its captures.
#
#   powershell -ExecutionPolicy Bypass -File scripts/capture_r4_contributions.ps1 -Phase measure
#
# Phases:
#   equivalence  the shipped full image, both scenes, twice (R4.1
#                instrumentation-off equivalence; run once before and once
#                after the instrumentation lands)
#   benchmark    L0 whole-frame timing, both scenes, twice (R4.9 cost gate);
#                the SSGI benchmark line lands in benchmark.txt
#   sources      R4.13 living-room diagnosis: path-traced references and L0
#                candidates with only the sun or only the sky (three seeds, 512
#                samples, as R3), and L0 with the cache's direct page split
#                by source.  Measurement only.
#   measure      the whole R4.1 matrix: repeats, owner isolation, owner
#                removal and ray coverage, for configurations S, L and L0
#   determinism  R5.1: full captures at frames 1, 32, 128 and 256, three runs
#                each, for the -Config configurations; compare hashes.txt
#
# benchmark without -Config keeps the R4 form (L0, runs a/b).  With -Config it
# interleaves the named configurations over runs a/b/c (R5.1 overhead gate).
param(
    [Parameter(Mandatory = $true)][ValidateSet('equivalence', 'measure', 'benchmark', 'sources', 'determinism')][string]$Phase,
    [string]$Label = '',
    # One scene per invocation keeps each foreground run short; both is the default.
    [ValidateSet('cornell', 'living-room', 'both')][string]$Scene = 'both',
    # Configurations to run (R4.5: L0 is the gate, S and L are kept as
    # control and diagnostic).  Comma-separated, since -File passes one string.
    [string]$Config = 'S,L,L0',
    # Write into an existing directory (the second scene of a split run).
    [string]$Output = '',
    [int]$TimeoutSeconds = 300,
    [switch]$SourceOwnerRepeats,
    # Only runs whose name matches this regex, so a slow matrix can be split
    # across foreground calls instead of running as one long one.
    [string]$Only = ''
)
$ErrorActionPreference = 'Stop'
$configNames = @($Config -split '[,\s]+' | Where-Object { $_ })
foreach ($c in $configNames) { if (@('S', 'L', 'L0') -cnotcontains $c) { throw ('Unknown configuration: ' + $c) } }

$repo = Split-Path $PSScriptRoot -Parent
$engine = Join-Path $repo 'bin/Release/engine.exe'
if (!(Test-Path -LiteralPath $engine)) { throw ('Release engine not built: ' + $engine) }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Output) {
    $output = (Resolve-Path -LiteralPath $Output).Path
} else {
    $output = Join-Path $repo ('tmp/r4-contributions/' + $stamp + '-' + $Phase + $(if ($Label) { '-' + $Label } else { '' }))
    if (Test-Path -LiteralPath $output) { throw ($output + ' already exists') }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
}

$scenes = @(
    @{ name = 'cornell';     scene = 'gi_cornell_box.json';       camera = '0 2 3.5 0 0' },
    @{ name = 'living-room'; scene = 'living_room_showcase.json'; camera = '0 1.6 -3 0 3.14' }
) | Where-Object { $Scene -eq 'both' -or $_.name -eq $Scene }

# R4.1 owner table.  field_exhausted contributes 0 by construction, so it has
# a coverage run but no radiance runs.
$forwardOwners = @('sun_diffuse', 'sun_specular', 'env_diffuse', 'env_specular', 'emission', 'background', 'emitter_diffuse', 'emitter_specular')
$configurations = @(
    @{ name = 'S'; variables = @{}; rays = @('ray_screen_hit', 'ray_exit', 'ray_exhausted', 'ray_unusable_hit', 'ray_portal'); coverageOnly = @() },
    @{ name = 'L'; variables = @{ MIRABILIS_LUMEN_LITE = '1' };
       rays = @('ray_screen_hit', 'ray_screen_hit_cache', 'ray_screen_hit_uncached', 'ray_portal', 'ray_field_cache', 'ray_field_uncovered', 'ray_field_exit', 'ray_primary_cache_sky');
       coverageOnly = @('ray_field_exhausted') },
    # R4.5 gate: radiosity off, so the cache holds one bounce and matches the
    # depth-2 references.
    @{ name = 'L0'; variables = @{ MIRABILIS_LUMEN_LITE = '1'; MIRABILIS_SURFACE_CACHE_RADIOSITY = '0' };
       rays = @('ray_screen_hit', 'ray_screen_hit_cache', 'ray_screen_hit_uncached', 'ray_portal', 'ray_field_cache', 'ray_field_uncovered', 'ray_field_exit', 'ray_primary_cache_sky');
       coverageOnly = @('ray_field_exhausted') }
) | Where-Object { $configNames -contains $_.name }

# Provenance (section 7.1).  The tree may be dirty during R4 work, so the
# exact diff is kept beside the captures rather than only a flag saying so.
Push-Location $repo
try {
    $revision = git rev-parse HEAD
    $dirty = @(git status --short)
    $tag = if ($Scene -eq 'both') { '' } else { '-' + $Scene }
    git diff HEAD --binary | Set-Content -Encoding utf8 (Join-Path $output ('worktree' + $tag + '.diff'))
    $untracked = @(git ls-files --others --exclude-standard)
    $untracked | Set-Content (Join-Path $output ('untracked' + $tag + '.txt'))
    foreach ($file in $untracked) {
        if ($file -like 'tmp/*') { continue }
        $target = Join-Path (Join-Path $output ('untracked' + $tag)) $file
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $file -Destination $target
    }
} finally { Pop-Location }
@(
    'Phase: ' + $Phase
    'Scene: ' + $Scene
    'Revision: ' + $revision
    'Dirty: ' + $(if ($dirty.Count) { 'yes (worktree.diff, untracked/)' } else { 'no' })
    'Engine SHA-256: ' + (Get-FileHash -LiteralPath $engine -Algorithm SHA256).Hash
    'Invocation: powershell -ExecutionPolicy Bypass -File scripts/capture_r4_contributions.ps1 -Phase ' + $Phase + ' -Scene ' + $Scene + ' -Config ' + ($configNames -join ',') + $(if ($Label) { ' -Label ' + $Label } else { '' })
) | Set-Content (Join-Path $output ('manifest' + $tag + '.txt'))
Get-ChildItem -LiteralPath (Join-Path $repo 'shaders') -Filter *.spv | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
} | Set-Content (Join-Path $output ('shader-sha256' + $tag + '.txt'))

$lastScene = Join-Path $repo 'assets/scenes/.last_scene'
$lastSceneBytes = if (Test-Path -LiteralPath $lastScene) { [IO.File]::ReadAllBytes($lastScene) } else { $null }
function Assert-LastScene {
    $now = if (Test-Path -LiteralPath $lastScene) { [IO.File]::ReadAllBytes($lastScene) } else { $null }
    if (($null -eq $now) -ne ($null -eq $lastSceneBytes) -or
        ($null -ne $now -and [Convert]::ToBase64String($now) -ne [Convert]::ToBase64String($lastSceneBytes))) {
        if ($null -ne $lastSceneBytes) { [IO.File]::WriteAllBytes($lastScene, $lastSceneBytes) }
        else { Remove-Item -LiteralPath $lastScene -Force }
        throw '.last_scene was modified by a capture; original restored, run abandoned'
    }
}

function Clear-EngineEnvironment {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
}

$saved = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' } | ForEach-Object { $saved[$_.Name] = $_.Value }

function Invoke-Capture([string]$name, [hashtable]$variables) {
    if ($Only -and $name -notmatch $Only) { return }
    Clear-EngineEnvironment
    $trace = $variables.ContainsKey('MIRABILIS_TEST_TRACE')
    if ($trace) {
        $variables['MIRABILIS_CAPTURE'] = Join-Path $output $name
    } else {
        $variables['MIRABILIS_RASTER_CAPTURE'] = Join-Path $output $name
        $variables['MIRABILIS_SSGI_CAPTURE'] = Join-Path $output ($name + '-ssgi')
    }
    foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$variables[$key], 'Process') }
    ($variables.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Name + '=' + $_.Value }) |
        Set-Content (Join-Path $output ($name + '.env.txt'))
    $log = Join-Path $output ($name + '.log')
    $err = Join-Path $output ($name + '.stderr.log')
    Write-Host ('Running ' + $name)
    $process = Start-Process -FilePath $engine -WorkingDirectory (Join-Path $repo 'bin/Release') `
        -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $handle = $process.Handle
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit(30 * 1000) | Out-Null
        Clear-EngineEnvironment
        Assert-LastScene
        throw ($name + ' exceeded ' + $TimeoutSeconds + ' seconds')
    }
    $process.Refresh()
    Clear-EngineEnvironment
    Assert-LastScene
    $text = ((Get-Content $log, $err -Raw -ErrorAction SilentlyContinue) -join "`n")
    if ($process.ExitCode -ne 0) { throw ($name + ' exited ' + $process.ExitCode + '; inspect ' + $log) }
    if ($text -match 'GI TEST FAIL|Validation Error|\[ERROR:|VUID-|Device lost') { throw ($name + ' reported an error; inspect ' + $log) }
    if ($text -notmatch 'GI bounded run complete:') { throw ($name + ' did not complete its frame budget') }
    if ($trace) {
        if ($text -notmatch 'GI capture .*nonfinite=0\b') { throw ($name + ' produced non-finite pixels or no trace capture') }
        foreach ($suffix in @('.pfm', '.direct.pfm', '.indirect.pfm', '.txt')) {
            $file = Join-Path $output ($name + $suffix)
            if (!(Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -eq 0) { throw ('Missing artifact: ' + $file) }
        }
        return
    }
    if ($text -notmatch 'Raster capture .*nonfinite=0\b') { throw ($name + ' produced non-finite pixels or no raster capture') }
    foreach ($suffix in @('.pfm', '.txt', '-ssgi.indirect.pfm', '-ssgi.raw.pfm', '-ssgi.txt')) {
        $file = Join-Path $output ($name + $suffix)
        if (!(Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -eq 0) { throw ('Missing artifact: ' + $file) }
    }
    $sidecar = Get-Content -Raw (Join-Path $output ($name + '.txt'))
    if ($sidecar -notmatch 'Dimensions: 960 x 540') { throw ($name + ': draw extent is not 960x540; the comparison is blocked') }
    if ($sidecar -notmatch 'Trace environment map: 0') { throw ($name + ': not in R3.1 environment parity') }
    if ($variables['MIRABILIS_SURFACE_CACHE_RADIOSITY'] -eq '0' -and $sidecar -notmatch 'Surface-cache radiosity: 0') {
        throw ($name + ': R4.5 gate run, but the sidecar does not record radiosity off')
    }
    # The engine's own record of what it kept must be what was asked for.
    $recorded = if ($sidecar -match 'R4 contributions: ([^\r\n]*)') { @($Matches[1].Trim() -split ' ' | Sort-Object) } else { @() }
    $requested = if ($variables.ContainsKey('MIRABILIS_R4_CONTRIBUTIONS')) {
        @($variables['MIRABILIS_R4_CONTRIBUTIONS'] -split ',' | Sort-Object) } else { @('all') }
    if (($recorded -join ' ') -ne ($requested -join ' ')) {
        throw ($name + ': sidecar records R4 contributions "' + ($recorded -join ' ') + '", requested "' + ($requested -join ' ') + '"')
    }
}

function Get-BaseVariables($case, [int]$frames) {
    $variables = @{
        MIRABILIS_TEST_FRAMES = [string]$frames
        MIRABILIS_TEST_SCENE = $case.scene
        MIRABILIS_TEST_CAMERA = $case.camera
        MIRABILIS_MATCH_REFERENCE = '1'
        MIRABILIS_SSGI_TRACE_ENVIRONMENT_MAP = '0'
    }
    return $variables
}

try {
    foreach ($case in $scenes) {
        if ($Phase -eq 'sources') {
            if ($case.scene -match 'sponza') { throw 'Sponza path tracing is forbidden on this hardware' }
            foreach ($source in @('sun', 'sky')) {
                foreach ($seed in @(1337, 2026, 90210)) {
                    Invoke-Capture ($case.name + '-' + $source + '-reference-seed' + $seed) @{
                        MIRABILIS_TEST_FRAMES = '512'
                        MIRABILIS_TEST_SCENE = $case.scene
                        MIRABILIS_TEST_CAMERA = $case.camera
                        MIRABILIS_TEST_TRACE = '1'
                        MIRABILIS_TEST_SEED = [string]$seed
                        MIRABILIS_SOURCE_STATE = $revision + ' (dirty; see worktree diff)'
                        MIRABILIS_R4_LIGHT_SOURCES = $source
                    }
                }
                $owners = @('ray_screen_hit', 'ray_screen_hit_uncached', 'ray_field_cache', 'ray_field_exit', 'ray_field_uncovered',
                    'sun_diffuse', 'sun_specular', 'env_specular', 'background')
                foreach ($frames in @(300, 332, 364)) {
                    $v = Get-BaseVariables $case $frames
                    $v['MIRABILIS_LUMEN_LITE'] = '1'; $v['MIRABILIS_SURFACE_CACHE_RADIOSITY'] = '0'
                    $v['MIRABILIS_R4_LIGHT_SOURCES'] = $source
                    Invoke-Capture ($case.name + '-' + $source + '-L0-full-f' + $frames) $v
                }
                foreach ($owner in $owners) {
                    $v = Get-BaseVariables $case 300
                    $v['MIRABILIS_LUMEN_LITE'] = '1'; $v['MIRABILIS_SURFACE_CACHE_RADIOSITY'] = '0'
                    $v['MIRABILIS_R4_LIGHT_SOURCES'] = $source
                    $v['MIRABILIS_R4_CONTRIBUTIONS'] = $owner
                    Invoke-Capture ($case.name + '-' + $source + '-L0-only-' + $owner) $v
                }
                # The cache's direct page with one source, everything else
                # lit as usual: how much of ray_field_cache each source is.
                foreach ($owner in @('', 'ray_field_cache', 'ray_screen_hit')) {
                    $v = Get-BaseVariables $case 300
                    $v['MIRABILIS_LUMEN_LITE'] = '1'; $v['MIRABILIS_SURFACE_CACHE_RADIOSITY'] = '0'
                    $v['MIRABILIS_R4_CACHE_SOURCES'] = $source
                    if ($owner) { $v['MIRABILIS_R4_CONTRIBUTIONS'] = $owner }
                    Invoke-Capture ($case.name + '-cache-' + $source + '-L0-' + $(if ($owner) { 'only-' + $owner } else { 'full-f300' })) $v
                    if ($owner -and $SourceOwnerRepeats) {
                        foreach ($frames in @(332, 364)) {
                            $v['MIRABILIS_TEST_FRAMES'] = [string]$frames
                            Invoke-Capture ($case.name + '-cache-' + $source + '-L0-only-' + $owner + '-f' + $frames) $v
                        }
                    }
                }
            }
            # Card coverage, reported by the engine itself.
            $v = Get-BaseVariables $case 60
            $v['MIRABILIS_LUMEN_LITE'] = '1'; $v['MIRABILIS_SURFACE_CACHE_RADIOSITY'] = '0'
            $v['MIRABILIS_SURFACE_CACHE_COVERAGE'] = '1'
            Invoke-Capture ($case.name + '-L0-card-coverage') $v
            continue
        }
        if ($Phase -eq 'benchmark') {
            $explicit = $PSBoundParameters.ContainsKey('Config')
            # Interleaved in the order -Config names them.
            $benchConfigs = if ($explicit) { foreach ($n in $configNames) { $configurations | Where-Object { $_.name -eq $n } } } else { @($configurations | Where-Object { $_.name -eq 'L0' }) }
            foreach ($run in $(if ($explicit) { @('a', 'b', 'c') } else { @('a', 'b') })) {
                foreach ($cfg in $benchConfigs) {
                    $v = Get-BaseVariables $case 300
                    foreach ($key in $cfg.variables.Keys) { $v[$key] = $cfg.variables[$key] }
                    $v['MIRABILIS_SSGI_BENCHMARK'] = '1'
                    $name = $case.name + '-' + $cfg.name + '-benchmark-' + $run
                    Invoke-Capture $name $v
                    $line = Select-String -Path (Join-Path $output ($name + '.log')) -Pattern 'SSGI benchmark:' | Select-Object -Last 1
                    if (!$line) { throw ('No benchmark line for ' + $name) }
                    Add-Content (Join-Path $output 'benchmark.txt') ($name + ': ' + $line.Line.Trim())
                }
            }
            continue
        }
        if ($Phase -eq 'determinism') {
            foreach ($cfg in $configurations) {
                foreach ($frames in @(1, 32, 128, 256)) {
                    foreach ($run in @('a', 'b', 'c')) {
                        $v = Get-BaseVariables $case $frames
                        foreach ($key in $cfg.variables.Keys) { $v[$key] = $cfg.variables[$key] }
                        Invoke-Capture ($case.name + '-' + $cfg.name + '-full-f' + $frames + '-' + $run) $v
                    }
                }
            }
            continue
        }
        if ($Phase -eq 'equivalence') {
            foreach ($run in @('a', 'b')) {
                Invoke-Capture ($case.name + '-S-full-eq' + $run) (Get-BaseVariables $case 300)
            }
            continue
        }
        foreach ($cfg in $configurations) {
            $prefix = $case.name + '-' + $cfg.name
            $owners = $forwardOwners + $cfg.rays
            function New-Run([int]$frames) {
                $v = Get-BaseVariables $case $frames
                foreach ($key in $cfg.variables.Keys) { $v[$key] = $cfg.variables[$key] }
                return $v
            }
            foreach ($frames in @(300, 332, 364)) {
                Invoke-Capture ($prefix + '-full-f' + $frames) (New-Run $frames)
            }
            foreach ($owner in $owners) {
                $v = New-Run 300
                $v['MIRABILIS_R4_CONTRIBUTIONS'] = $owner
                Invoke-Capture ($prefix + '-only-' + $owner) $v
                $v = New-Run 300
                $v['MIRABILIS_R4_CONTRIBUTIONS'] = (($owners + $cfg.coverageOnly) | Where-Object { $_ -ne $owner }) -join ','
                Invoke-Capture ($prefix + '-without-' + $owner) $v
            }
            # R4.12: the forward pass's emitter cache lookups, 1 at a hit and
            # 0.5 at a miss (Lumen-lite configurations only; S has no cache).
            if ($cfg.name -ne 'S') {
                $v = New-Run 300
                $v['MIRABILIS_R4_CONTRIBUTIONS'] = 'emitter_diffuse'
                $v['MIRABILIS_R4_RAY_COVERAGE'] = '1'
                Invoke-Capture ($prefix + '-coverage-emitter_cache') $v
            }
            foreach ($ray in ($cfg.rays + $cfg.coverageOnly)) {
                $v = New-Run 300
                $v['MIRABILIS_R4_CONTRIBUTIONS'] = $ray
                $v['MIRABILIS_R4_RAY_COVERAGE'] = '1'
                Invoke-Capture ($prefix + '-coverage-' + $ray) $v
            }
        }
    }
    Get-ChildItem -LiteralPath $output -File -Filter *.pfm | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
    } | Set-Content (Join-Path $output 'hashes.txt')
    Write-Host ('R4 captures complete: ' + $output)
} finally {
    Clear-EngineEnvironment
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
