# Effectiveness comparison: Quick vs Quick2 on the REAL USB disk (default disk 2,
# the Lexar at F:). Measures the anytime-wipe property that matters for defeating
# file-carving recovery: after a time-boxed cancel, how much CONTIGUOUS intact data
# is left, and where.
#
# Method: clean the disk, plant a 512-byte signature at every 8 MB boundary across
# the whole device (forward-only seeks, fast), then run the wiper time-boxed for the
# same number of seconds in each mode and re-scan the markers. A marker survives iff
# the block/chunk containing it was not yet overwritten. The longest run of
# consecutive surviving markers x 8 MB approximates the largest intact region a
# carver could still recover.
#
# MUST run elevated. WIPES disk 2. Writes results to test\effectiveness-results.txt.
param([int]$Disk = 2, [int]$Seconds = 12, [int]$SpacingMB = 8)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\dist\QuickWiper.exe'
$out = Join-Path $PSScriptRoot 'effectiveness-results.txt'
$sig = [Text.Encoding]::ASCII.GetBytes('QWMARK__')   # 8 bytes, then 8-byte LE offset, then pad
$spacing = [int64]$SpacingMB * 1MB
$sector  = 512

function Log($m) { Add-Content -Path $out -Value $m; Write-Host $m }
Set-Content -Path $out -Value "Quick vs Quick2 effectiveness (disk $Disk, ${Seconds}s, markers every ${SpacingMB}MB)"
Log "started: $(Get-Date -Format o)"

function Open-Raw() {
    $path = "\\.\PhysicalDrive$Disk"
    $h = [System.IO.FileStream]::new($path, 'Open', 'ReadWrite', 'ReadWrite')
    return $h
}
function Get-DiskSize() {
    (Get-Disk -Number $Disk).Size
}
function Clean-Disk() {
    $f = Join-Path $env:TEMP "qw-clean-$Disk.txt"
    Set-Content $f "select disk $Disk`r`nclean`r`nexit`r`n" -Encoding ascii
    & "$env:SystemRoot\System32\diskpart.exe" /s $f | Out-Null
    Remove-Item $f -Force
    Start-Sleep 2
}

function Plant($size) {
    $marker = New-Object byte[] $sector
    $h = Open-Raw
    try {
        $off = [int64]0
        while ($off + $sector -le $size) {
            [Array]::Clear($marker, 0, $sector)
            [Array]::Copy($sig, 0, $marker, 0, $sig.Length)
            [BitConverter]::GetBytes($off).CopyTo($marker, $sig.Length)
            # fill the rest with a fixed pattern so random data won't reproduce it
            for ($i = 16; $i -lt $sector; $i++) { $marker[$i] = [byte](($off + $i) -band 0xFF) }
            $h.Seek($off, 'Begin') | Out-Null
            $h.Write($marker, 0, $sector)
            $off += $spacing
        }
        $h.Flush($true)
        return [int]([math]::Floor($size / $spacing))
    } finally { $h.Close() }
}

function Scan($size) {
    $h = Open-Raw
    $buf = New-Object byte[] $sector
    $survivors = New-Object System.Collections.Generic.List[int]
    try {
        $idx = 0; $off = [int64]0
        while ($off + $sector -le $size) {
            $h.Seek($off, 'Begin') | Out-Null
            $read = $h.Read($buf, 0, $sector)
            $ok = $true
            for ($i = 0; $i -lt $sig.Length; $i++) { if ($buf[$i] -ne $sig[$i]) { $ok = $false; break } }
            if ($ok) {
                $expOff = [BitConverter]::ToInt64($buf, $sig.Length)
                if ($expOff -ne $off) { $ok = $false }
            }
            if ($ok) { $survivors.Add($idx) }
            $idx++; $off += $spacing
        }
    } finally { $h.Close() }
    return $survivors
}

function LongestRun($survivors) {
    $best = 0; $cur = 0; $prev = -2
    foreach ($s in $survivors) {
        if ($s -eq $prev + 1) { $cur++ } else { $cur = 1 }
        if ($cur -gt $best) { $best = $cur }
        $prev = $s
    }
    return $best
}

function RunWipe($mode) {
    $tmp = Join-Path $env:TEMP "qw-eff-$mode.txt"
    $p = Start-Process -FilePath $exe -Wait -PassThru -RedirectStandardOutput $tmp `
         -ArgumentList @('wipe','--disk',"$Disk",'--mode',$mode,'--seconds',"$Seconds",'--yes')
    $text = Get-Content $tmp -Raw
    $avg = '?'; $m = [regex]::Matches($text, 'avg\s+([\d.]+)'); if ($m.Count) { $avg = $m[$m.Count-1].Groups[1].Value }
    return $avg
}

try {
    $size = Get-DiskSize
    Log "disk size: $size bytes ($([math]::Round($size/1GB,1)) GB)"
    foreach ($mode in 'quick','quick2') {
        Clean-Disk
        $total = Plant $size
        $before = (Scan $size).Count
        $avg = RunWipe $mode
        $surv = Scan $size
        $killed = $total - $surv.Count
        $run = LongestRun $surv
        Log ("{0,-7}: planted {1}, before-wipe intact {2}, killed {3} ({4:N1}%), survivors {5}, longest intact run {6} markers (~{7} MB), avg {8} MB/s" -f `
            $mode, $total, $before, $killed, (100.0*$killed/$total), $surv.Count, $run, ($run*$SpacingMB), $avg)
    }
    Clean-Disk
    Log "done: $(Get-Date -Format o)"
} catch {
    Log "EXCEPTION: $($_.Exception.Message)"
}
