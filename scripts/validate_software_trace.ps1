param([ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=Join-Path $repo ('tmp/gi-checkpoints/validation-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object { $saved[$_.Name]=$_.Value }
$cases=@(
    @{name='raster-shadow'; scene='shadow_showcase.json'; frames=16; raster=$true},
    @{name='raster-buffer-lab'; scene='screen_space_buffer_lab.json'; frames=16; raster=$true},
    @{name='raster-ssgi'; scene='gi_cornell_box.json'; frames=64; raster=$true; ssgiCapture=$true; camera='0 2 3.5 0 0'},
    @{name='raster-ssgi-repeat'; scene='gi_cornell_box.json'; frames=64; raster=$true; ssgiCapture=$true; camera='0 2 3.5 0 0'},
    @{name='ssao-buffer-lab'; scene='ssao_buffer_lab.json'; frames=16; raster=$true; debugView=7; requireSsao=$true},
    @{name='editor-resume'; scene='gi_cornell_box.json'; frames=8; raster=$true; editorResume=$true},
    @{name='fallback'; scene='shadow_showcase.json'; frames=16; disable=$true},
    @{name='mode-resize-cycle'; scene='shadow_showcase.json'; frames=20; cycle=$true},
    @{name='invalidation'; scene='gi_portal_pair.json'; frames=20; invalidation=$true},
    @{name='dark'; scene='gi_cornell_box_dark.json'; frames=8; black=$true; camera='0 2 3.5 0 0'},
    @{name='open'; scene='gi_open_box.json'; frames=32; lit=$true},
    @{name='open-repeat'; scene='gi_open_box.json'; frames=32; lit=$true},
    @{name='cornell'; scene='gi_cornell_box.json'; frames=64; lit=$true; camera='0 2 3.5 0 0'},
    @{name='materials'; scene='gi_material_lab.json'; frames=64; depth=4; camera='0 2.5 7 -0.08 0'},
    @{name='portal-pair'; scene='gi_portal_pair.json'; frames=32; camera='0 2.5 7 -0.08 0'},
    @{name='portal-cycle'; scene='gi_portal_cycle.json'; frames=32; camera='0 2 2 0 0'},
    @{name='portal-multi'; scene='gi_portal_multi_pair.json'; frames=32; camera='0 2.5 7 -0.08 0'},
    @{name='portal-sun'; scene='gi_portal_sun.json'; frames=64; lit=$true; camera='0 2 3.5 0 0'},
    @{name='portal-sun-disabled'; scene='gi_portal_sun.json'; frames=8; black=$true; limit=0; camera='0 2 3.5 0 0'},
    @{name='portal-emitter'; scene='gi_portal_emitter.json'; frames=64; lit=$true; camera='0 2 3.5 0 0'},
    @{name='portal-emitter-disabled'; scene='gi_portal_emitter.json'; frames=8; black=$true; limit=0; camera='0 2 3.5 0 0'}
)
Push-Location $repo
try {
    $revision=git rev-parse HEAD
    if($LASTEXITCODE -ne 0) { throw 'Cannot record source revision' }
    git status --short | Set-Content (Join-Path $output 'worktree.txt')
    $patchPath=Join-Path $output 'working.patch'
    git diff --binary "--output=$patchPath"
    if($LASTEXITCODE -ne 0) { throw 'Cannot record working patch' }
    $sources=Get-ChildItem src,shaders,assets/scenes -File -Recurse | Where-Object { $_.Extension -ne '.spv' }
    $sources+=Get-Item assets/textures/gi_color_test.ppm,CMakeLists.txt,scripts/validate_software_trace.ps1
    $manifest=$sources | Sort-Object FullName | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,$_.FullName.Substring($repo.Length+1) }
    $manifestPath=Join-Path $output 'source-sha256.txt'
    $manifest | Set-Content $manifestPath
    $sourceState=$revision+'; working-source SHA256 '+(Get-FileHash $manifestPath).Hash
    Get-FileHash "bin/$Configuration/engine.exe",shaders/path_trace.comp.spv | Format-List | Out-File (Join-Path $output 'binary-sha256.txt')
    $lastSceneHash=(Get-FileHash assets/scenes/.last_scene).Hash
    foreach($case in $cases) {
        Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
        $env:VK_LAYER_VALIDATE_SYNC='1'
        $env:MIRABILIS_SOURCE_STATE=$sourceState
        $env:MIRABILIS_TEST_FRAMES=[string]$case.frames
        $env:MIRABILIS_TEST_SCENE=$case.scene
        if(!$case.raster) { $env:MIRABILIS_TEST_TRACE='1' }
        if($case.disable) { $env:MIRABILIS_TEST_DISABLE_TRACE='1' }
        if($case.cycle) { $env:MIRABILIS_TEST_CYCLE='1' }
        if($case.invalidation) { $env:MIRABILIS_TEST_INVALIDATION='1' }
        if($case.editorResume) { $env:MIRABILIS_TEST_EDITOR_RESUME='1' }
        if($case.requireSsao) { $env:MIRABILIS_TEST_REQUIRE_SSAO='1' }
        if($case.ContainsKey('debugView')) { $env:MIRABILIS_RENDER_DEBUG_VIEW=[string]$case.debugView }
        if($case.black) { $env:MIRABILIS_EXPECT_BLACK='1' }
        if($case.lit) { $env:MIRABILIS_EXPECT_LIT='1' }
        if($case.depth) { $env:MIRABILIS_TEST_DEPTH=[string]$case.depth }
        if($case.ContainsKey('limit')) { $env:MIRABILIS_TEST_PORTAL_LIMIT=[string]$case.limit }
        if($case.camera) { $env:MIRABILIS_TEST_CAMERA=$case.camera }
        if(!$case.raster -and !$case.disable) { $env:MIRABILIS_CAPTURE=Join-Path $output $case.name }
        if($case.ssgiCapture) { $env:MIRABILIS_SSGI_CAPTURE=Join-Path $output $case.name }
        $log=Join-Path $output ($case.name+'.log')
        $err=Join-Path $output ($case.name+'.stderr.log')
        Write-Output ('Running '+$case.name)
        $process=Start-Process -FilePath (Join-Path $repo "bin/$Configuration/engine.exe") -WorkingDirectory (Join-Path $repo "bin/$Configuration") -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
        $processHandle=$process.Handle # Retain the handle so Windows PowerShell preserves ExitCode.
        if(!$process.WaitForExit(60000)) { $process.Kill(); throw ($case.name+' exceeded 60 seconds') }
        $process.Refresh()
        $text=(Get-Content $log,$err -Raw) -join "`n"
        if($process.ExitCode -ne 0 -or $text -match 'GI TEST FAIL|Validation Error|\[ERROR:|VUID-') { throw ($case.name+' failed; inspect '+$log) }
        if($text -notmatch 'GI bounded run complete:') { throw ($case.name+' exited before completing its frame budget') }
        if($case.disable -and $text -notmatch 'GI fallback:') { throw 'Fallback was not exercised' }
        if($case.editorResume -and $text -notmatch 'GI editor resume:.* PASS') { throw 'Editor resume was not exercised' }
        if($case.requireSsao -and $text -notmatch 'GI test: ssao=active') { throw 'SSAO was not exercised' }
        if($case.cycle -and $text -notmatch 'GI lifecycle: minimized and restore requested') { throw 'Minimize/restore was not exercised' }
        if(!$case.raster -and !$case.disable -and !(Test-Path -LiteralPath (Join-Path $output ($case.name+'.pfm')))) { throw 'Capture was not produced' }
        if($case.ssgiCapture -and !(Test-Path -LiteralPath (Join-Path $output ($case.name+'.indirect.pfm')))) { throw 'SSGI capture was not produced' }
        if((Get-FileHash assets/scenes/.last_scene).Hash -ne $lastSceneHash) { throw 'Test changed remembered scene' }
        Write-Output ('PASS '+$case.name)
    }
    if((Get-FileHash (Join-Path $output 'open.pfm')).Hash -ne (Get-FileHash (Join-Path $output 'open-repeat.pfm')).Hash) { throw 'Deterministic captures differ' }
    if((Get-FileHash (Join-Path $output 'raster-ssgi.indirect.pfm')).Hash -ne (Get-FileHash (Join-Path $output 'raster-ssgi-repeat.indirect.pfm')).Hash) { throw 'Raster SSGI deterministic captures differ' }
    Write-Output ('PASS deterministic capture equality; all '+$cases.Count+' cases. Artifacts: '+$output)
} finally {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
    foreach($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    Pop-Location
}
