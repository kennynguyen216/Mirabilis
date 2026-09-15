# Captures the fixed images and timings the raster material model milestones
# are compared against (docs/raster_material_model_design.md, Milestone 0).
# Raster captures and benchmarks use the Release build; the sandbox path-trace
# capture uses Debug, like the validation harness it sits beside.
param(
    [Parameter(Mandatory=$true)][string]$Label,
    # A finished scripts/validate_software_trace.ps1 artifact directory to copy
    # in, so its path-trace captures are hashed with the rest.
    [string]$HarnessArtifacts='',
    # Another label under tmp/material-baseline to compare hashes against.
    [string]$Baseline='',
    [switch]$SkipBenchmark
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$root=Join-Path $repo 'tmp/material-baseline'
$output=Join-Path $root $Label
if(Test-Path -LiteralPath $output) { throw ($output+' already exists') }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object { $saved[$_.Name]=$_.Value }

function Clear-EngineEnvironment {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
}

function Invoke-Engine([string]$name,[string]$configuration,[hashtable]$variables,[int]$timeoutSeconds,[bool]$hidden) {
    Clear-EngineEnvironment
    foreach($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key,[string]$variables[$key],'Process') }
    $log=Join-Path $output ($name+'.log')
    $err=Join-Path $output ($name+'.stderr.log')
    Write-Host ('Running '+$name)
    $arguments=@{
        FilePath=(Join-Path $repo "bin/$configuration/engine.exe")
        WorkingDirectory=(Join-Path $repo "bin/$configuration")
        PassThru=$true
        RedirectStandardOutput=$log
        RedirectStandardError=$err
    }
    if($hidden) { $arguments.WindowStyle='Hidden' }
    $process=Start-Process @arguments
    $processHandle=$process.Handle # Retain the handle so Windows PowerShell preserves ExitCode.
    if(!$process.WaitForExit($timeoutSeconds*1000)) { $process.Kill(); throw ($name+' exceeded '+$timeoutSeconds+' seconds') }
    $process.Refresh()
    $text=(Get-Content $log,$err -Raw) -join "`n"
    Clear-EngineEnvironment
    if($process.ExitCode -ne 0 -or $text -match 'GI TEST FAIL|Validation Error|\[ERROR:|VUID-') { throw ($name+' failed; inspect '+$log) }
    if($text -notmatch 'GI bounded run complete:') { throw ($name+' exited before completing its frame budget') }
    return $text
}

function Assert-Repeat([string]$first,[string]$repeat) {
    if(!(Test-Path -LiteralPath $first) -or !(Test-Path -LiteralPath $repeat)) { throw ('Capture was not produced: '+$first) }
    if((Get-FileHash -LiteralPath $first).Hash -ne (Get-FileHash -LiteralPath $repeat).Hash) { throw ('Repeat capture differs: '+$first) }
    # Equal bytes, so the repeat adds nothing to keep.
    Remove-Item -LiteralPath $repeat
}

Push-Location $repo
try {
    $revision=git rev-parse HEAD
    if($LASTEXITCODE -ne 0) { throw 'Cannot record source revision' }
    @('Revision: '+$revision,'Worktree:')+(git status --short) | Set-Content (Join-Path $output 'source.txt')
    Get-FileHash bin/Debug/engine.exe,bin/Release/engine.exe | Format-List | Out-File (Join-Path $output 'binary-sha256.txt')

    $rasterCases=@(
        @{name='sponza'; scene='sponza_showcase.json'; camera='0 4.2 0 -0.05 1.5708'},
        @{name='material-lab'; scene='gi_material_lab.json'; camera='0 2.5 7 -0.08 0'},
        @{name='shadow'; scene='shadow_showcase.json'; camera='0 6 32 -0.2 0'}
    )
    foreach($case in $rasterCases) {
        foreach($ssgi in @('on','off')) {
            $base='raster-'+$case.name+'-ssgi-'+$ssgi
            foreach($suffix in @('','-repeat')) {
                $variables=@{
                    VK_LAYER_VALIDATE_SYNC='1'
                    MIRABILIS_TEST_FRAMES='64'
                    MIRABILIS_TEST_SCENE=$case.scene
                    MIRABILIS_TEST_CAMERA=$case.camera
                    MIRABILIS_RASTER_CAPTURE=(Join-Path $output ($base+$suffix))
                }
                if($ssgi -eq 'off') { $variables.MIRABILIS_SSGI_DISABLE='1' }
                Invoke-Engine ($base+$suffix) 'Release' $variables 300 $true | Out-Null
            }
            Assert-Repeat (Join-Path $output ($base+'.pfm')) (Join-Path $output ($base+'-repeat.pfm'))
        }
    }

    # sandbox.json is the one scene here whose floor uses the built-in floor
    # material, which Milestone 1 changes for the path tracer.
    foreach($suffix in @('','-repeat')) {
        $variables=@{
            VK_LAYER_VALIDATE_SYNC='1'
            MIRABILIS_TEST_FRAMES='32'
            MIRABILIS_TEST_SCENE='sandbox.json'
            MIRABILIS_TEST_CAMERA='0 5 12 -0.35 0'
            MIRABILIS_TEST_TRACE='1'
            MIRABILIS_CAPTURE=(Join-Path $output ('trace-sandbox'+$suffix))
        }
        Invoke-Engine ('trace-sandbox'+$suffix) 'Debug' $variables 300 $true | Out-Null
    }
    foreach($component in @('.pfm','.direct.pfm','.indirect.pfm')) {
        Assert-Repeat (Join-Path $output ('trace-sandbox'+$component)) (Join-Path $output ('trace-sandbox-repeat'+$component))
    }
    Remove-Item -LiteralPath (Join-Path $output 'trace-sandbox-repeat.bmp')

    if(!$SkipBenchmark) {
        $results=@()
        foreach($scene in @('sponza_showcase.json','sandbox.json','portal_bhop_course.json')) {
            $variables=@{
                MIRABILIS_TEST_FRAMES='400'
                MIRABILIS_SSGI_BENCHMARK='1'
                MIRABILIS_TEST_SCENE=$scene
            }
            $text=Invoke-Engine ('benchmark-'+[IO.Path]::GetFileNameWithoutExtension($scene)) 'Release' $variables 300 $false
            $line=($text -split "`n" | Where-Object { $_ -match 'SSGI benchmark:' } | Select-Object -Last 1)
            if(!$line) { throw ('No benchmark line for '+$scene) }
            $results+=($scene+': '+$line.Trim())
        }
        $results | Set-Content (Join-Path $output 'benchmark.txt')
        $results | ForEach-Object { Write-Host $_ }
    }

    if($HarnessArtifacts) {
        Copy-Item -LiteralPath $HarnessArtifacts -Destination (Join-Path $output 'harness') -Recurse
    }

    $hashes=Get-ChildItem -LiteralPath $output -Recurse -File -Filter *.pfm | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,$_.FullName.Substring($output.Length+1).Replace('\','/')
    }
    $hashes | Set-Content (Join-Path $output 'hashes.txt')

    if($Baseline) {
        $baselineHashes=@{}
        Get-Content (Join-Path (Join-Path $root $Baseline) 'hashes.txt') | ForEach-Object {
            $parts=$_ -split '  ',2; $baselineHashes[$parts[1]]=$parts[0]
        }
        $comparison=@()
        $current=@{}
        foreach($entry in $hashes) {
            $parts=$entry -split '  ',2; $current[$parts[1]]=$parts[0]
            if(!$baselineHashes.ContainsKey($parts[1])) { $comparison+=('NEW        '+$parts[1]) }
            elseif($baselineHashes[$parts[1]] -eq $parts[0]) { $comparison+=('IDENTICAL  '+$parts[1]) }
            else { $comparison+=('DIFFERENT  '+$parts[1]) }
        }
        foreach($path in $baselineHashes.Keys) {
            if(!$current.ContainsKey($path)) { $comparison+=('MISSING    '+$path) }
        }
        $comparison=$comparison | Sort-Object { $_.Substring(11) }
        $comparison | Set-Content (Join-Path $output ('compare-'+$Baseline+'.txt'))
        $comparison | Where-Object { $_ -notlike 'IDENTICAL*' } | ForEach-Object { Write-Host $_ }
        Write-Host ('Compared with '+$Baseline+': '+@($comparison | Where-Object { $_ -like 'IDENTICAL*' }).Count+' identical of '+$comparison.Count)
    }
    Write-Host ('Material baseline complete. Artifacts: '+$output)
} finally {
    Clear-EngineEnvironment
    foreach($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    Pop-Location
}
