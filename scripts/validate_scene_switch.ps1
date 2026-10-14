# Run the same in-engine scene replacement path as the editor's scene picker.
param(
    [string]$SourceScene = 'lighting_calibration.json',
    [string]$TargetScene = 'sponza_showcase.json',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
if (!$Output) { $Output = Join-Path $repo 'tmp/scene-switch' }
$output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$saved = @{}
Get-ChildItem Env: | Where-Object Name -Like 'MIRABILIS_*' | ForEach-Object { $saved[$_.Name] = $_.Value }
$remembered = (Get-FileHash "$repo/assets/scenes/.last_scene").Hash
try {
    Get-ChildItem Env: | Where-Object Name -Like 'MIRABILIS_*' | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
    $env:MIRABILIS_TEST_SCENE = $SourceScene
    $env:MIRABILIS_TEST_SCENE_SWITCH = $TargetScene
    $env:MIRABILIS_TEST_FRAMES = '100'
    $env:MIRABILIS_LUMEN_LITE = '1'
    $env:MIRABILIS_SSGI_PRESET = '2'
    $env:MIRABILIS_SSGI_PROBES = '0'
    $env:MIRABILIS_SSGI_SCREEN_TRACE = '0'
    $env:MIRABILIS_SURFACE_CACHE_RADIOSITY = '1'
    $env:MIRABILIS_R4_UNCAP_FRAMES = '1'
    $p = Start-Process "$repo/bin/$Configuration/engine.exe" -WorkingDirectory "$repo/bin/$Configuration" -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/switch.log" -RedirectStandardError "$output/switch.stderr.log"
    $handle = $p.Handle
    if (!$p.WaitForExit(600000)) { $p.Kill(); throw 'Scene switch timed out' }
    $p.Refresh()
    $logs = (Get-Content "$output/switch.log", "$output/switch.stderr.log" -Raw) -join "`n"
    if ($p.ExitCode -ne 0 -or $logs -match 'Unhandled renderer exception|Detected Vulkan error|VUID-|GI TEST FAIL') {
        throw "Scene switch failed (exit $($p.ExitCode)); logs: $output"
    }
    if (!$logs.Contains("GI scene switch loaded: $TargetScene") -or
        $logs -notmatch 'GI bounded run complete: frames=100 renderer=raster') {
        throw "Scene switch did not complete; logs: $output"
    }
    if ((Get-FileHash "$repo/assets/scenes/.last_scene").Hash -ne $remembered) { throw '.last_scene changed' }
    Write-Output "PASS: $SourceScene -> $TargetScene; 100 rendered frames; exit 0"
} finally {
    Get-ChildItem Env: | Where-Object Name -Like 'MIRABILIS_*' | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
