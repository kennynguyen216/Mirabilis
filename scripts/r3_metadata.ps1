# Writes the provenance files for an R3 capture directory.
#
# Separate from capture_r3_references.ps1 so it can be exercised without a GPU,
# an engine binary, or a capture: the first real run produced a manifest whose
# every dynamic line was split in two, a source.txt collapsed onto one line,
# and a UTF-16 binary hash wrapped at the host's console width. None of that
# was visible to tests that checked hand-written fixtures instead of what this
# code actually emits.
#
# Two rules hold everything here together:
#
#   1. Every key/value record is built with -f, or with explicitly parenthesised
#      concatenation. PowerShell binds "," tighter than "+", so
#          @('Commit: ' + $revision, 'Branch: ' + $branch)
#      parses as 'Commit: ' + ($revision, 'Branch: ') + $branch -- a string plus
#      an array -- which is what split every dynamic line in the first run.
#
#   2. Nothing goes through Format-List, Format-Table or Out-File. Those format
#      for a console of some width and wrap accordingly, so the bytes on disk
#      depend on the window that happened to run the script.
#
# No Set-StrictMode here: this file is dot-sourced, so anything it sets would
# silently change the execution semantics of the rest of the caller's script.

function Write-HashRecord {
    # One line, always: <64 hex>  <name>. ASCII so the file is byte-identical
    # whatever the host's default encoding is.
    param([string]$Path, [string]$FilePath, [string]$Label)
    $hash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash
    Set-Content -LiteralPath $Path -Value ('{0}  {1}' -f $hash, $Label) -Encoding ascii
}

function Get-R3InvocationDescription {
    # $MyInvocation.Line is empty when a script is started with -File, so the
    # first run recorded a blank invocation. Rebuild it from the parameters
    # instead: deterministic, and it actually reproduces the run.
    #
    # Direct invocation, NOT -File. -File passes every argument as a string, so
    # "-Seeds 1337,2026,90210" arrives as one string and fails to bind to
    # [int[]]$Seeds -- the recorded command would not have run.
    param([int[]]$Seeds, [int]$Frames, [int]$TimeoutSeconds, [bool]$Diagnostic)
    $text = ('& .\scripts\capture_r3_references.ps1' +
             (' -Seeds {0} -Frames {1} -TimeoutSeconds {2}' -f
                ($Seeds -join ','), $Frames, $TimeoutSeconds))
    if ($Diagnostic) { $text = $text + ' -Diagnostic' }
    return $text
}

function Write-R3Metadata {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][string]$Repo,
        [Parameter(Mandatory = $true)][string]$Revision,
        [Parameter(Mandatory = $true)][string]$Branch,
        [string[]]$Dirty = @(),
        [Parameter(Mandatory = $true)][int[]]$Seeds,
        [Parameter(Mandatory = $true)][int]$Frames,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$Invocation,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][array]$Scenes,
        [string]$CapturedAt = ''
    )

    $dirtyLines = @($Dirty)
    $isDirty = $dirtyLines.Count -gt 0
    $accepted = if ($isDirty) { 'NO - diagnostic only, worktree was dirty' } else { 'yes' }
    if (!$CapturedAt) { $CapturedAt = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K') }

    # Each element is its own parenthesised expression. Without the parentheses
    # the commas bind tighter than the concatenations and the array turns into
    # a ragged mixture of strings and sub-arrays.
    $manifest = @(
        'R3 reference capture manifest',
        '',
        ('Acceptable as an R3 reference: {0}' -f $accepted),
        ('Captured (local): {0}' -f $CapturedAt),
        ('Commit: {0}' -f $Revision),
        ('Branch: {0}' -f $Branch),
        ('Dirty tree: {0}' -f $(if ($isDirty) { 'YES' } else { 'no' })),
        '',
        ('Invocation: {0}' -f $Invocation),
        'Script: scripts/capture_r3_references.ps1',
        ('Script SHA-256: {0}' -f (Get-FileHash -LiteralPath (Join-Path $Repo 'scripts/capture_r3_references.ps1') -Algorithm SHA256).Hash),
        'Validator: scripts/validate_r3_capture.py',
        ('Validator SHA-256: {0}' -f (Get-FileHash -LiteralPath (Join-Path $Repo 'scripts/validate_r3_capture.py') -Algorithm SHA256).Hash),
        'Metadata writer: scripts/r3_metadata.ps1',
        ('Metadata writer SHA-256: {0}' -f (Get-FileHash -LiteralPath (Join-Path $Repo 'scripts/r3_metadata.ps1') -Algorithm SHA256).Hash),
        'Regions: r3_regions.json beside this file is the frozen copy validation uses;',
        '  its SHA-256 is in regions-sha256.txt. The repository copy is not consulted.',
        ('Seeds: {0}' -f ($Seeds -join ', ')),
        ('Frames (accumulated samples per capture): {0}' -f $Frames),
        ('Per-run timeout (s): {0}' -f $TimeoutSeconds),
        'Build configuration: Release',
        'Engine: bin/Release/engine.exe (SHA-256 in binary-sha256.txt)',
        '',
        'Capture format',
        '  <name>.pfm           combined linear radiance, PFM, 3 channels, 32-bit float',
        '  <name>.direct.pfm    direct component only, same format',
        '  <name>.indirect.pfm  indirect component only, same format',
        '  <name>.bmp           24-bit preview, tone mapped by the engine (NOT for metrics)',
        '  <name>.txt           per-capture metadata written by the engine',
        '  <name>.env.txt       the exact environment that run was given',
        '  <name>.log/.stderr.log  preserved stdout and stderr',
        '',
        'Colour and units',
        '  The .pfm files are LINEAR HDR radiance. NO TONE MAPPING, no exposure and',
        '  no sRGB transfer has been applied to them. Every metric in section 7.3 is',
        '  computed on these linear values. The .bmp is the only tone-mapped artifact',
        '  and exists to look at, not to measure.',
        '',
        'Row orientation',
        '  PFM header scale is -1.0, meaning little-endian samples. Rows are stored',
        '  bottom-to-top per the PFM format: the first row in the file is the BOTTOM',
        '  row of the image. A reader must flip vertically before indexing, as',
        '  scripts/pfm_to_png.py read_pfm() and scripts/validate_r3_capture.py do.',
        '  This is verified per capture against the engine-written .bmp, not assumed;',
        '  see the orientation lines in validation.txt.',
        '  Region coordinates in r3_regions.json are normalised 0..1 from the TOP-LEFT',
        '  of the image as displayed, i.e. after that flip.',
        '',
        'Scenes and cameras'
    )
    foreach ($case in $Scenes) {
        $manifest += ('  {0}: {1}  camera {2}' -f $case.name, $case.scene, $case.camera)
    }
    $manifest += @('', 'Worktree at capture time:')
    $manifest += $dirtyLines
    Set-Content -LiteralPath (Join-Path $Output 'manifest.txt') -Value $manifest -Encoding ascii

    $source = @(
        ('Commit: {0}' -f $Revision),
        ('Branch: {0}' -f $Branch),
        ('Dirty tree: {0}' -f $(if ($isDirty) { 'YES' } else { 'no' })),
        'Worktree:'
    )
    $source += $dirtyLines
    Set-Content -LiteralPath (Join-Path $Output 'source.txt') -Value $source -Encoding ascii

    # One record, one line, no formatter in the way.
    Write-HashRecord (Join-Path $Output 'binary-sha256.txt') $EnginePath 'bin/Release/engine.exe'

    $shaderRecords = @(Get-ChildItem (Join-Path $Repo 'shaders') -File | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
    })
    Set-Content -LiteralPath (Join-Path $Output 'shader-sha256.txt') -Value $shaderRecords -Encoding ascii

    $sceneRecords = @(foreach ($case in $Scenes) {
        $scenePath = Join-Path $Repo ('assets/scenes/' + $case.scene)
        '{0}  {1}' -f (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash, $case.scene
    })
    Set-Content -LiteralPath (Join-Path $Output 'scene-sha256.txt') -Value $sceneRecords -Encoding ascii

    $regionsSource = Join-Path $Repo 'scripts/r3_regions.json'
    $regionsFrozen = Join-Path $Output 'r3_regions.json'
    Copy-Item -LiteralPath $regionsSource -Destination $regionsFrozen
    Write-HashRecord (Join-Path $Output 'regions-sha256.txt') $regionsFrozen 'r3_regions.json'

    return $regionsFrozen
}
