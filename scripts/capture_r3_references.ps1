# Captures the matched path-traced references R3 requires
# (docs/lumen_lite_design.md, sections 7.1-7.4 and R3).
#
# Foreground only, Release only, Cornell and the living room only.  Sponza
# reference generation is forbidden on this laptop and this script will not do
# it however it is called.  Nothing here runs hidden or in the background: the
# engine window is visible for every run, so a hang is something you can see.
#
#   powershell -ExecutionPolicy Bypass -File scripts/capture_r3_references.ps1
#
# Artifacts land in a new tmp/r3-references/<timestamp> directory.  Nothing
# existing is overwritten, so historical evidence survives a re-run.
param(
    # Seeds to render each scene at.  Section 7.2 wants repeated seeds so the
    # reference's own noise can be quantified before anything is compared
    # against it.  Three distinct seeds is the floor, not a suggestion: two
    # cannot separate a spread from a single outlier.
    [int[]]$Seeds = @(1337, 2026, 90210),
    # Accumulated path-trace samples per capture; one per frame.  Identical
    # across every scene and seed, or the captures are not comparable.
    [int]$Frames = 512,
    # Per-run wall-clock ceiling.  A run that exceeds it is killed and the
    # whole capture fails: a partial reference is worse than none.
    [int]$TimeoutSeconds = 900,
    # Permit a dirty worktree.  The captures are then labelled diagnostic-only
    # in the manifest and may not be cited as accepted R3 references.
    [switch]$Diagnostic
)
$ErrorActionPreference = 'Stop'

# Section 7.1 is only satisfiable if the inputs are themselves well formed.
$distinctSeeds = @($Seeds | Select-Object -Unique)
if ($distinctSeeds.Count -lt 3) {
    throw ('R3 requires at least three distinct seeds; got ' + $distinctSeeds.Count +
           ' (' + ($Seeds -join ', ') + ')')
}
if ($distinctSeeds.Count -ne $Seeds.Count) { throw ('Duplicate seeds: ' + ($Seeds -join ', ')) }
if ($Frames -le 0) { throw ('Frames must be positive; got ' + $Frames) }
if ($TimeoutSeconds -le 0) { throw ('TimeoutSeconds must be positive; got ' + $TimeoutSeconds) }

$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'r3_metadata.ps1')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$output = Join-Path $repo ('tmp/r3-references/' + $stamp)
if (Test-Path -LiteralPath $output) { throw ($output + ' already exists') }

# Section 7.4: these two, and only these two.  Sponza is absent by
# construction, not by a flag someone can flip.
$scenes = @(
    @{ name = 'cornell';     scene = 'gi_cornell_box.json';        camera = '0 2 3.5 0 0' },
    @{ name = 'living-room'; scene = 'living_room_showcase.json';  camera = '0 1.6 -3 0 3.14' }
)
foreach ($case in $scenes) {
    if ($case.scene -match 'sponza') { throw 'Sponza path tracing is forbidden on this hardware' }
}

$engine = Join-Path $repo 'bin/Release/engine.exe'
if (!(Test-Path -LiteralPath $engine)) { throw ('Release engine not built: ' + $engine) }

Push-Location $repo
try {
    $revision = git rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source revision' }
    $dirty = @(git status --short)
    # Fail closed.  A capture taken from a tree that does not exist in history
    # cannot be reproduced from its own record, so it is not a reference, and
    # the safe default is to refuse rather than to footnote it.
    if ($dirty.Count -gt 0 -and !$Diagnostic) {
        throw ("Worktree is dirty; accepted R3 references require a clean tree.`n" +
               ($dirty -join "`n") +
               "`nCommit the tree, or re-run with -Diagnostic to produce " +
               'captures explicitly labelled diagnostic-only.')
    }
} finally { Pop-Location }

New-Item -ItemType Directory -Path $output -Force | Out-Null

# The editor's scene state belongs to whoever was last using the editor.  The
# engine only writes it when MIRABILIS_TEST_FRAMES is unset, so a capture run
# should never touch it -- but "should never" is the reason to keep the bytes
# rather than just a hash: if a run does modify it, the original is put back
# before anything else happens, and the capture still fails.
$lastScene = Join-Path $repo 'assets/scenes/.last_scene'
$lastSceneExisted = Test-Path -LiteralPath $lastScene
$lastSceneBytes = if ($lastSceneExisted) { [IO.File]::ReadAllBytes($lastScene) } else { $null }

function Test-LastScene {
    $exists = Test-Path -LiteralPath $lastScene
    if ($exists -ne $lastSceneExisted) { return $false }
    if (!$exists) { return $true }
    $current = [IO.File]::ReadAllBytes($lastScene)
    if ($current.Length -ne $lastSceneBytes.Length) { return $false }
    for ($i = 0; $i -lt $current.Length; ++$i) {
        if ($current[$i] -ne $lastSceneBytes[$i]) { return $false }
    }
    return $true
}

function Restore-LastScene {
    if ($lastSceneExisted) { [IO.File]::WriteAllBytes($lastScene, $lastSceneBytes) }
    elseif (Test-Path -LiteralPath $lastScene) { Remove-Item -LiteralPath $lastScene -Force }
}

function Assert-LastScene([string]$stage) {
    if (Test-LastScene) { return }
    Restore-LastScene
    throw ('.last_scene was modified by ' + $stage + '; the original bytes have been ' +
           'restored and the capture is abandoned')
}

$captureError = $null
$lastSceneViolation = $false
$saved = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object { $saved[$_.Name] = $_.Value }

function Clear-EngineEnvironment {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'MIRABILIS_*' -or $_.Name -eq 'VK_LAYER_VALIDATE_SYNC' } | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
}

function Invoke-Capture([string]$name, [hashtable]$variables) {
    # Every run starts from a cleared environment, so a variable left over from
    # an earlier session cannot quietly change what is being captured.
    Clear-EngineEnvironment
    foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$variables[$key], 'Process') }
    ($variables.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Name + '=' + $_.Value }) |
        Set-Content (Join-Path $output ($name + '.env.txt'))

    $log = Join-Path $output ($name + '.log')
    $err = Join-Path $output ($name + '.stderr.log')
    Write-Host ('Running ' + $name + ' (foreground, window visible)')
    $process = Start-Process -FilePath $engine -WorkingDirectory (Join-Path $repo 'bin/Release') `
        -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $handle = $process.Handle  # Retained so Windows PowerShell keeps ExitCode.
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        # Kill, then wait for the kill to land before looking at anything the
        # process might still be writing. Throwing straight after Kill() races
        # a still-dying engine: the .last_scene check below could read the file
        # mid-write, or the restore could be overwritten a moment later.
        $process.Kill()
        if (!$process.WaitForExit(30 * 1000)) {
            throw ($name + ' exceeded ' + $TimeoutSeconds +
                   ' seconds and did not terminate within 30 seconds of being killed')
        }
        Clear-EngineEnvironment
        Assert-LastScene ('timed-out capture ' + $name)
        throw ($name + ' exceeded ' + $TimeoutSeconds + ' seconds')
    }
    $process.Refresh()
    Clear-EngineEnvironment

    $text = ((Get-Content $log, $err -Raw -ErrorAction SilentlyContinue) -join "`n")
    if ($process.ExitCode -ne 0) { throw ($name + ' exited ' + $process.ExitCode + '; inspect ' + $log) }
    if ($text -match 'GI TEST FAIL|Validation Error|\[ERROR:|VUID-|Device lost') { throw ($name + ' reported an error; inspect ' + $log) }
    if ($text -notmatch 'GI bounded run complete:') { throw ($name + ' exited before completing its frame budget') }
    # capture_path_trace aborts on a non-finite pixel, so reaching here already
    # implies none; the log line is checked anyway because a silent change to
    # that guard must not pass unnoticed, and validate_r3_capture.py decodes
    # every file afterwards regardless.
    if ($text -notmatch 'nonfinite=0\b') { throw ($name + ' produced non-finite pixels; inspect ' + $log) }
    # After every engine process, not only at the end: a run that touched the
    # editor's scene state must not be followed by five more that hide it.
    Assert-LastScene ('capture ' + $name)
    return $text
}

Push-Location $repo
try {
    $branch = git rev-parse --abbrev-ref HEAD
    $accepted = if ($dirty.Count -gt 0) { 'NO - diagnostic only, worktree was dirty' } else { 'yes' }

    # Provenance is written by scripts/r3_metadata.ps1 so it can be exercised
    # without an engine or a GPU. The first real run shipped a manifest with
    # every dynamic line split in two and a console-width-wrapped UTF-16 hash,
    # none of which the fixture-based tests could see.
    $invocation = Get-R3InvocationDescription -Seeds $Seeds -Frames $Frames `
        -TimeoutSeconds $TimeoutSeconds -Diagnostic:$Diagnostic.IsPresent
    $regionsFrozen = Write-R3Metadata -Output $output -Repo $repo -Revision $revision `
        -Branch $branch -Dirty $dirty -Seeds $Seeds -Frames $Frames `
        -TimeoutSeconds $TimeoutSeconds -Invocation $invocation -EnginePath $engine `
        -Scenes $scenes

    foreach ($case in $scenes) {

        foreach ($seed in $Seeds) {
            $name = $case.name + '-seed' + $seed
            # Identical across scenes and seeds except the seed itself, so any
            # difference between two captures is the seed and nothing else.
            $variables = @{
                VK_LAYER_VALIDATE_SYNC = '1'
                MIRABILIS_TEST_FRAMES  = $Frames
                MIRABILIS_TEST_SCENE   = $case.scene
                MIRABILIS_TEST_CAMERA  = $case.camera
                MIRABILIS_TEST_TRACE   = '1'
                MIRABILIS_TEST_SEED    = $seed
                MIRABILIS_SOURCE_STATE = $revision + $(if ($dirty.Count -gt 0) { ' (dirty)' } else { '' })
                MIRABILIS_CAPTURE      = (Join-Path $output $name)
            }
            $text = Invoke-Capture $name $variables

            if ($text -notmatch ('GI test seed: ' + [regex]::Escape([string]$seed) + ' \(MIRABILIS_TEST_SEED\)')) {
                throw ($name + ' did not run at the requested seed; inspect ' + $name + '.log')
            }
            foreach ($artifact in @('.pfm', '.direct.pfm', '.indirect.pfm', '.txt', '.bmp')) {
                $file = Join-Path $output ($name + $artifact)
                if (!(Test-Path -LiteralPath $file)) { throw ('Missing artifact: ' + $file) }
                if ((Get-Item -LiteralPath $file).Length -eq 0) { throw ('Empty artifact: ' + $file) }
            }
        }
    }

    Get-ChildItem -LiteralPath $output -File -Filter *.pfm | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
    } | Set-Content (Join-Path $output 'hashes.txt')

    Assert-LastScene 'the capture set'

    # Decode everything that was just written.  A file the engine produced and
    # nobody read back is not evidence, and the cross-seed spread has to exist
    # before any candidate is compared against these images.
    Write-Host ''
    Write-Host 'Validating captures and measuring reference noise'
    & python (Join-Path $repo 'scripts/validate_r3_capture.py') $output `
        --regions $regionsFrozen
    $validation = $LASTEXITCODE
    if ($validation -ne 0) {
        throw ('Capture validation failed (exit ' + $validation + '); see ' +
               (Join-Path $output 'validation.txt'))
    }

    Write-Host ''
    Write-Host ('R3 references complete: ' + $output)
    Write-Host ('Acceptable as an R3 reference: ' + $accepted)
} catch {
    # Held, not rethrown here: the finally block below still has to run, and a
    # .last_scene violation discovered there must be reported alongside this
    # failure rather than replacing it.
    $captureError = $_
} finally {
    Clear-EngineEnvironment
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    # Unconditional: an exception on any path above must not leave the editor's
    # scene state changed.
    if (!(Test-LastScene)) {
        Restore-LastScene
        $lastSceneViolation = $true
    }
    Pop-Location
}

# A modified .last_scene fails the run. Warning and exiting zero would mean the
# one file this script promises not to touch could be changed by a "successful"
# capture, and nothing downstream would know.
if ($lastSceneViolation) {
    $message = '.last_scene was modified during the run; original bytes restored'
    if ($captureError) {
        throw ($captureError.ToString() + [Environment]::NewLine + $message)
    }
    throw $message
}
if ($captureError) { throw $captureError }
