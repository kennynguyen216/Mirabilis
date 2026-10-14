# Foreground raster-only R6 timing/validation runs; no scene or preference saves.
# Control compares the original cache schedule, legacy portal shading, or plain
# raster/SSGI. Release provides timing evidence; Debug enables validation.
param(
    [string]$Scene = 'living_room_showcase.json',
    [string]$Camera = '4.740 1.700 -2.947 -0.133 -14.670',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$Motion, [switch]$Validation,
    [ValidateSet('radiosity','portal','raster')][string]$Control = 'radiosity',
    [switch]$Single, [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
if (!$Output) { $Output = Join-Path $repo ('tmp/r6/metrics-' + $Scene.Replace('.json','') + '-' + $Configuration + '-' + $Control + '-' + $(if ($Motion) {'motion'} else {'static'}) + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($Output)
if ($Validation -and $Configuration -ne 'Debug') { throw 'Validation requires Debug' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
Get-FileHash "$repo/bin/$Configuration/engine.exe", "$repo/shaders/*.spv" |
    ForEach-Object { $_.Hash + '  ' + $_.Path } | Set-Content "$output/binaries-sha256.txt"
$saved = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' -or $_.Name -like 'VK_KHRONOS_VALIDATION_*' } | ForEach-Object { $saved[$_.Name] = $_.Value }
$remembered = Get-FileHash "$repo/assets/scenes/.last_scene"
try {
    foreach ($run in $(if ($Single) {@('a')} else {@('a','b','c')})) {
        foreach ($mode in $(if ($Single) {@('candidate')} else {@('control','candidate')})) {
            Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
            $name = "$mode-$run"
            $env:MIRABILIS_TEST_SCENE = $Scene
            $env:MIRABILIS_TEST_CAMERA = $Camera
            $env:MIRABILIS_TEST_FRAMES = '300'
            $env:MIRABILIS_MATCH_REFERENCE = '1'
            $env:MIRABILIS_LUMEN_LITE = '1'
            $env:MIRABILIS_SSGI_PRESET = '2'
            $env:MIRABILIS_SSGI_PROBES = '0'
            $env:MIRABILIS_SSGI_HZB = '0'
            $env:MIRABILIS_SSGI_TRACE_ENVIRONMENT_MAP = '0'
            $env:MIRABILIS_SURFACE_CACHE_RADIOSITY = '1'
            if ($mode -eq 'control') {
                if ($Control -eq 'radiosity') { $env:MIRABILIS_R6_RADIOSITY_CONTROL='1' }
                if ($Control -eq 'portal') { $env:MIRABILIS_R6_PORTAL_GI_CONTROL='1' }
                if ($Control -eq 'raster') { $env:MIRABILIS_LUMEN_LITE='0' }
            }
            $env:MIRABILIS_R4_UNCAP_FRAMES = '1'
            $env:MIRABILIS_SSGI_BENCHMARK = '1'
            $env:MIRABILIS_R4_FRAME_TIME_LOG = Join-Path $output "$name.csv"
            if ($Motion) {
                $env:MIRABILIS_CINEMATIC = '300'
                $env:MIRABILIS_CINEMATIC_OUTPUT = Join-Path $output "$name-motion"
                $env:MIRABILIS_R4_NO_CINEMATIC_CAPTURE = '1'
                $env:MIRABILIS_R4_INDOOR_SWEEP = '1'
            }
            if ($Validation) {
                $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC='true'
                $env:VK_KHRONOS_VALIDATION_DEBUG_ACTION='VK_DBG_LAYER_ACTION_LOG_MSG'
                $env:VK_KHRONOS_VALIDATION_REPORT_FLAGS='info,warn,perf,error'
                $env:VK_KHRONOS_VALIDATION_LOG_FILENAME=Join-Path $output "$name-validation.txt"
            }
            Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' -or $_.Name -like 'VK_KHRONOS_VALIDATION_*' } | Sort-Object Name | ForEach-Object { $_.Name + '=' + $_.Value } | Set-Content "$output/$name.env.txt"
            Write-Host "Running foreground $Scene $Configuration $name motion=$Motion validation=$Validation"
            $process = Start-Process -FilePath "$repo/bin/$Configuration/engine.exe" -WorkingDirectory "$repo/bin/$Configuration" -PassThru -RedirectStandardOutput "$output/$name.log" -RedirectStandardError "$output/$name.stderr.log"
            $handle = $process.Handle
            $timeoutMs = if ($Configuration -eq 'Debug') { 900000 } else { 300000 }
            if (!$process.WaitForExit($timeoutMs)) { $process.Kill(); throw "$name timed out" }
            $process.Refresh()
            $logs = (Get-Content "$output/$name.log","$output/$name.stderr.log" -Raw) -join "`n"
            if ($process.ExitCode -ne 0 -or $logs -match 'GI TEST FAIL|Validation Error:|VUID-|Device lost') { throw "$name failed: $output" }
            if ($Validation) {
                $layer = Get-Content "$output/$name-validation.txt" -Raw
                if ($layer -notmatch 'Synchronization' -or $layer -match 'VUID-|SYNC-|ERROR|WARNING') { throw 'Synchronization validation failed or was not enabled' }
            }
            if ($logs -notmatch 'GI bounded run complete:.*renderer=raster') { throw "$name incomplete" }
            if ($Configuration -eq 'Release' -and $logs -notmatch 'GI benchmark focused samples: 295/295') {
                throw "$name lost foreground focus; timing is invalid"
            }
            if ((Get-FileHash "$repo/assets/scenes/.last_scene").Hash -ne $remembered.Hash) { throw '.last_scene changed' }
        }
    }
} finally {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' -or $_.Name -like 'VK_KHRONOS_VALIDATION_*' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
