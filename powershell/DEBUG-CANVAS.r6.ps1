#requires -Version 7.0
using namespace System
using namespace System.Diagnostics
using namespace System.Management.Automation
using namespace System.Management.Automation.Runspaces
using namespace System.Threading

[CmdletBinding()]
param(
    [switch] $Ui,
    [string] $AssemblyPath = (Join-Path $PSScriptRoot 'bin\DirectPort.PowerShell.dll'),
    [string] $AtlasPng = $env:DIRECTPORT_ATLAS_PNG,
    [string] $MetricsJson = $env:DIRECTPORT_ATLAS_METRICS
)

if ([string]::IsNullOrWhiteSpace($AtlasPng) -or
    [string]::IsNullOrWhiteSpace($MetricsJson) -or
    -not (Test-Path -LiteralPath $AtlasPng) -or
    -not (Test-Path -LiteralPath $MetricsJson)) {
    throw 'Pass -AtlasPng and -MetricsJson, or set DIRECTPORT_ATLAS_PNG and DIRECTPORT_ATLAS_METRICS.'
}

# Keep the UI/message pump isolated exactly like r5. The parent owns teardown
# and forwards only the deliberately compact smoke output.
if (-not $Ui) {
    $rs = [RunspaceFactory]::CreateRunspace()
    $rs.ApartmentState = [ApartmentState]::STA
    $rs.ThreadOptions = [PSThreadOptions]::ReuseThread
    $rs.Open()
    $ps = [PowerShell]::Create()
    $ps.Runspace = $rs
    [void]$ps.AddCommand($PSCommandPath).AddParameter('Ui').AddParameter('AssemblyPath', $AssemblyPath).AddParameter('AtlasPng', $AtlasPng).AddParameter('MetricsJson', $MetricsJson)
    $async = $ps.BeginInvoke()
    try {
        [void]$async.AsyncWaitHandle.WaitOne()
        $output = $ps.EndInvoke($async)
        foreach ($line in $output) { [Console]::WriteLine($line) }
        if ($ps.Streams.Error.Count) { throw $ps.Streams.Error[0] }
    }
    finally {
        $ps.Dispose()
        $rs.Dispose()
    }
    return
}

enum CanvasMode { Touch; Pan; Colors; Charset; Grid; Noise }

[Reflection.Assembly]::LoadFrom($AssemblyPath) | Out-Null
$gpu = $null
$script:Cells = [uint32[]]::new(1)
$script:Columns = 1
$script:Rows = 1
$script:Mode = [CanvasMode]::Touch
$script:Frame = 0
$script:Scale = 1
$script:PanX = 0.0
$script:PanY = 0.0
$script:Dirty = $true
$script:WasAnimating = $false
$script:Ready = $false
$script:Rng = [Random]::new()
$script:NoiseBytes = [byte[]]::new(2)
$script:TouchX = [Collections.Generic.List[int]]::new()
$script:TouchY = [Collections.Generic.List[int]]::new()
$script:TouchAt = [Collections.Generic.List[long]]::new()
$script:LastTouchCell = -1
$script:LastResizeSerial = [uint64]::MaxValue
$script:LastFps = 0.0
$script:LastBuild = 0.0
$script:LastDropped = [uint64]0
$script:EmitSample = $true

$labels = @(
    @{ Text = ' 1 TOUCH '; Mode = [CanvasMode]::Touch },
    @{ Text = ' 2 PAN '; Mode = [CanvasMode]::Pan },
    @{ Text = ' 3 COLORS '; Mode = [CanvasMode]::Colors },
    @{ Text = ' 4 GLYPHS '; Mode = [CanvasMode]::Charset },
    @{ Text = ' 5 GRID '; Mode = [CanvasMode]::Grid },
    @{ Text = ' 6 NOISE '; Mode = [CanvasMode]::Noise }
)

function Set-Mode([CanvasMode] $Mode) {
    $wasAnimated = $script:Mode -in @([CanvasMode]::Charset, [CanvasMode]::Noise)
    $script:Mode = $Mode
    $isAnimated = $Mode -in @([CanvasMode]::Charset, [CanvasMode]::Noise)
    if ($wasAnimated -and -not $isAnimated) { $script:WasAnimating = $true }
    $script:Dirty = $true
    $script:EmitSample = $true
}

function Add-TouchCell([int] $X, [int] $Y) {
    if ($X -lt 0 -or $X -ge $script:Columns -or $Y -lt 1 -or $Y -ge $script:Rows) { return }
    $cell = $Y * $script:Columns + $X
    if ($cell -eq $script:LastTouchCell) { return }
    $script:LastTouchCell = $cell
    $script:TouchX.Add($X)
    $script:TouchY.Add($Y)
    $script:TouchAt.Add([Environment]::TickCount64)
    $script:Dirty = $true
}

function Put-Text([uint32[]] $Cells, [int] $Columns, [int] $Rows, [int] $X, [int] $Y,
                  [string] $Text, [int] $Foreground, [int] $Background) {
    if ($Y -lt 0 -or $Y -ge $Rows) { return }
    for ($i = 0; $i -lt $Text.Length -and ($X + $i) -lt $Columns; $i++) {
        if (($X + $i) -ge 0) {
            $Cells[$Y * $Columns + $X + $i] = [uint32](($Background -shl 24) -bor ($Foreground -shl 16) -bor [int]$Text[$i])
        }
    }
}

function Build-Cells([int] $ViewWidth, [int] $ViewHeight) {
    $cellWidth = 9 * $script:Scale
    $cellHeight = 18 * $script:Scale
    $columns = [Math]::Clamp([int][Math]::Floor($ViewWidth / $cellWidth), 1, 512)
    $rows = [Math]::Clamp([int][Math]::Floor($ViewHeight / $cellHeight), 2, 512)
    while (($columns * $rows) -gt 262144) { $rows-- }
    $shape = $columns -ne $script:Columns -or $rows -ne $script:Rows
    if ($shape) {
        $script:Columns = $columns
        $script:Rows = $rows
        $script:Cells = [uint32[]]::new($columns * $rows)
        $script:NoiseBytes = [byte[]]::new(2 * $columns * $rows)
    }
    else { [Array]::Clear($script:Cells, 0, $script:Cells.Length) }

    $cells = $script:Cells
    $mode = $script:Mode
    $px = [int][Math]::Floor($script:PanX)
    $py = [int][Math]::Floor($script:PanY)
    $centerX = [int]($columns / 2)
    $centerY = [int]($rows / 2)
    $band = [Math]::Max(1.0, $columns / 16.0)

    # Select the producer once per frame, never once per cell.
    if ($mode -eq [CanvasMode]::Grid) {
        for ($y = 1; $y -lt $rows; $y++) {
            $rowAt = $y * $columns
            for ($x = 0; $x -lt $columns; $x++) {
                $cp = 32; $fg = 15; $bg = if ((($x + $y) -band 1) -eq 0) { 8 } else { 0 }
                if ($x -eq 0 -or $y -eq 1 -or $x -eq ($columns - 1) -or $y -eq ($rows - 1)) { $bg = 1; $cp = 35 }
                if (($x % 10) -eq 0 -and (($y - 1) % 5) -eq 0) { $bg = 4; $cp = 79 }
                $cells[$rowAt + $x] = [uint32](($bg -shl 24) -bor ($fg -shl 16) -bor $cp)
            }
        }
    }
    elseif ($mode -eq [CanvasMode]::Colors) {
        for ($y = 1; $y -lt $rows; $y++) {
            $rowAt = $y * $columns; $cp = 65 + ($y % 26)
            for ($x = 0; $x -lt $columns; $x++) {
                $bg = [int][Math]::Floor($x / $band) % 16; $fg = ($bg + 8) % 16
                $cells[$rowAt + $x] = [uint32](($bg -shl 24) -bor ($fg -shl 16) -bor $cp)
            }
        }
    }
    elseif ($mode -eq [CanvasMode]::Charset) {
        for ($y = 1; $y -lt $rows; $y++) {
            $rowAt = $y * $columns; $fg = ($y % 15) + 1
            for ($x = 0; $x -lt $columns; $x++) {
                $cp = 33 + (($rowAt + $x + $script:Frame) % 94)
                $cells[$rowAt + $x] = [uint32](($fg -shl 16) -bor $cp)
            }
        }
    }
    elseif ($mode -eq [CanvasMode]::Noise) {
        # Entropy generation stays in one native call; PowerShell only packs cells.
        $script:Rng.NextBytes($script:NoiseBytes)
        for ($y = 1; $y -lt $rows; $y++) {
            $rowAt = $y * $columns
            for ($x = 0; $x -lt $columns; $x++) {
                $at = $rowAt + $x; $v = $script:NoiseBytes[2 * $at]
                if (($v -band 15) -eq 0) {
                    $cp = 33 + ($v % 90); $fg = $script:NoiseBytes[2 * $at + 1] -band 15
                    $cells[$at] = [uint32](($fg -shl 16) -bor $cp)
                }
            }
        }
    }
    elseif ($mode -eq [CanvasMode]::Pan) {
        for ($y = 1; $y -lt $rows; $y++) {
            $rowAt = $y * $columns; $wy = ($y - 1) - $py
            for ($x = 0; $x -lt $columns; $x++) {
                $cp = 32; $fg = 15; $bg = 0; $wx = $x - $px
                if ((([Math]::Abs($wx) -band 1) -eq 0) -eq (([Math]::Abs($wy) -band 1) -eq 0)) { $bg = 8 }
                if ($wx -eq 0 -and $wy -eq 0) { $bg = 4; $cp = 88 }
                elseif (($wx % 10) -eq 0 -and ($wy % 10) -eq 0) { $cp = 43 }
                if ($x -eq $centerX -and $y -eq $centerY) { $bg = 1; $cp = 64 }
                $cells[$rowAt + $x] = [uint32](($bg -shl 24) -bor ($fg -shl 16) -bor $cp)
            }
        }
    }

    # Oldest -> newest: normal last-write-wins makes the newest touch authoritative.
    $now = [Environment]::TickCount64
    for ($i = $script:TouchAt.Count - 1; $i -ge 0; $i--) {
        if (($now - $script:TouchAt[$i]) -ge 500) {
            $script:TouchAt.RemoveAt($i); $script:TouchX.RemoveAt($i); $script:TouchY.RemoveAt($i)
        }
    }
    for ($i = 0; $i -lt $script:TouchAt.Count; $i++) {
        $age = $now - $script:TouchAt[$i]
        if ($age -lt 100) { $bg = 14; $fg = 0; $cp = 88 }
        elseif ($age -lt 300) { $bg = 6; $fg = 15; $cp = 120 }
        else { $bg = 8; $fg = 15; $cp = 46 }
        $at = $script:TouchY[$i] * $columns + $script:TouchX[$i]
        if ($at -ge 0 -and $at -lt $cells.Length) { $cells[$at] = [uint32](($bg -shl 24) -bor ($fg -shl 16) -bor $cp) }
    }

    if ($mode -eq [CanvasMode]::Touch) {
        Put-Text $cells $columns $rows 3 3 ' CANVAS HOST: DIRECTPORT D3D12 ' 15 4
        $status = if ($script:TouchX.Count) {
            ' COORDS: [X:{0:D3} Y:{1:D3}] ' -f $script:TouchX[$script:TouchX.Count - 1], $script:TouchY[$script:TouchY.Count - 1]
        } else { ' WAITING FOR INPUT... ' }
        Put-Text $cells $columns $rows 3 5 $status 10 0
    }

    # Toolbar is itself cells; click ranges are reconstructed from these labels.
    $toolbarX = 0
    foreach ($button in $labels) {
        $selected = $button.Mode -eq $mode
        Put-Text $cells $columns $rows $toolbarX 0 $button.Text $(if ($selected) { 15 } else { 7 }) $(if ($selected) { 4 } else { 8 })
        $toolbarX += $button.Text.Length
    }
    if ($toolbarX -lt $columns) {
        $telemetry = ' {0}x{1} {2:0}fps {3:0.0}ms D{4} ' -f $columns, $rows, $script:LastFps, $script:LastBuild, $script:LastDropped
        Put-Text $cells $columns $rows $toolbarX 0 $telemetry 8 0
    }
    return $shape
}

$clock = [Stopwatch]::StartNew()
$frequency = [double][Stopwatch]::Frequency
$targetTicks = [long]($frequency / 120.0)
$nextFrame = [Stopwatch]::GetTimestamp()
$sampleAt = $nextFrame
$sampleFrames = 0
$buildTotal = 0.0
$publishValue = [uint64]0
$previousLeft = $false
$previousX = 0
$previousY = 0

try {
    $suffix = "${PID}_debugcanvas"
    $gpu = [DirectPort.PowerShell.GpuConsole]::new(
        1280, 720, 262144,
        "Global\D3D12_Texture_$suffix", "Global\D3D12_Fence_$suffix",
        $AtlasPng, $MetricsJson)
    $gpu.EnableManifest("D3D12_Producer_Manifest_$PID")
    $gpu.Show('DirectPort Debug Canvas r6', 1000, 650)

    while ($true) {
        $animated = $script:Mode -in @([CanvasMode]::Charset, [CanvasMode]::Noise)
        $live = $script:Dirty -or $animated -or ($script:TouchAt.Count -gt 0)
        $wait = if ($live) { 1 } else { 1000 }
        $state = $gpu.Pump([uint32]$wait)
        if (-not $state.Alive) { break }

        if ($state.ResizeSerial -ne $script:LastResizeSerial) {
            $script:LastResizeSerial = $state.ResizeSerial
            $script:Dirty = $true
            if ($script:Ready) { "RESIZE $($state.Width)x$($state.Height)" }
        }
        if ($state.KeyCode -ge 49 -and $state.KeyCode -le 54) { Set-Mode ([CanvasMode]($state.KeyCode - 49)) }
        elseif ($state.KeyCode -eq 27) { break }
        elseif ($state.KeyCode -eq 187 -or $state.KeyCode -eq 107) { $script:Scale = [Math]::Min(8, $script:Scale + 1); $script:Dirty = $true }
        elseif ($state.KeyCode -eq 189 -or $state.KeyCode -eq 109) { $script:Scale = [Math]::Max(1, $script:Scale - 1); $script:Dirty = $true }

        $columns = [Math]::Max(1, $script:Columns); $rows = [Math]::Max(2, $script:Rows)
        $cellX = [Math]::Clamp([int][Math]::Floor($state.MouseX / [Math]::Max(1.0, $state.Width / $columns)), 0, $columns - 1)
        $cellY = [Math]::Clamp([int][Math]::Floor($state.MouseY / [Math]::Max(1.0, $state.Height / $rows)), 0, $rows - 1)
        if ($state.LeftDown -and -not $previousLeft -and $cellY -eq 0) {
            $hitX = 0
            foreach ($button in $labels) {
                if ($cellX -ge $hitX -and $cellX -lt ($hitX + $button.Text.Length)) { Set-Mode $button.Mode; break }
                $hitX += $button.Text.Length
            }
        }
        elseif ($state.LeftDown -and $cellY -gt 0) {
            if ($script:Mode -eq [CanvasMode]::Pan -and $previousLeft) {
                $script:PanX -= ($state.MouseX - $previousX) / [Math]::Max(1.0, $state.Width / $columns)
                $script:PanY -= ($state.MouseY - $previousY) / [Math]::Max(1.0, $state.Height / $rows)
                $script:Dirty = $true
            }
            Add-TouchCell $cellX $cellY
        }
        if (-not $state.LeftDown) { $script:LastTouchCell = -1 }
        if ($state.WheelDelta -and $script:Mode -eq [CanvasMode]::Pan) {
            $script:PanY -= $state.WheelDelta / 120.0
            $script:Dirty = $true
        }
        $previousLeft = $state.LeftDown; $previousX = $state.MouseX; $previousY = $state.MouseY

        $nowTicks = [Stopwatch]::GetTimestamp()
        $animated = $script:Mode -in @([CanvasMode]::Charset, [CanvasMode]::Noise)
        $live = $script:Dirty -or $animated -or ($script:TouchAt.Count -gt 0)
        if (-not $live -or $nowTicks -lt $nextFrame) { continue }
        $buildAt = $nowTicks
        $shape = Build-Cells $state.Width $state.Height
        if ($shape -and $script:Ready) { "RESIZE $($script:Columns)x$($script:Rows)" }
        $publishValue++
        $submitted = $gpu.TryPresent($script:Cells, $script:Columns, $script:Rows, [single]$clock.Elapsed.TotalSeconds, $publishValue)
        $builtAt = [Stopwatch]::GetTimestamp()
        if ($submitted) {
            if (-not $script:Ready) { 'READY'; $script:Ready = $true }
            $script:Frame++
            $script:Dirty = $false
            $sampleFrames++
            $buildTotal += 1000.0 * ($builtAt - $buildAt) / $frequency
            if ($script:WasAnimating -and -not $animated) { 'IDLE'; $script:WasAnimating = $false }
            if ($animated) { $script:WasAnimating = $true }
        }
        if (($builtAt - $sampleAt) -ge $frequency) {
            $elapsed = ($builtAt - $sampleAt) / $frequency
            $script:LastFps = $sampleFrames / $elapsed
            $script:LastBuild = $buildTotal / [Math]::Max(1, $sampleFrames)
            $script:LastDropped = $gpu.DroppedFrames
            if ($script:EmitSample) {
                'FPS={0:0} BUILD={1:0.0}ms' -f $script:LastFps, $script:LastBuild
                $script:EmitSample = $false
            }
            $sampleAt = $builtAt; $sampleFrames = 0; $buildTotal = 0.0
        }
        $nextFrame = [Math]::Max($nextFrame + $targetTicks, $builtAt)
    }
}
finally {
    if ($null -ne $gpu) { $gpu.Dispose() }
    'CLOSED'
}
