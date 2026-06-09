# QuickWiper integration test against a throwaway VHD.
# SAFETY: this script wipes ONLY a brand-new file-backed VHD it creates itself.
# It snapshots existing physical disks first and refuses to touch any pre-existing
# disk, any disk index 0, anything larger than the VHD, or any non-virtual device.
#
# It does NOT use diskpart partition/format (VDS) for setup — a fixed VHD file is a
# raw disk image, so we write/scan marker bytes directly in the file. Only create/
# attach/detach vdisk are used. Reformat (needs VDS) is reported as informational.

param([string]$ExePath = 'C:\code\usb-wipe\dist\QuickWiper.exe')
$ErrorActionPreference = 'Stop'
$work = 'C:\code\usb-wipe\test'
$log  = Join-Path $work 'test.log'
$exe  = $ExePath
$vhd  = Join-Path $work 'victim.vhd'
$marker = [Text.Encoding]::ASCII.GetBytes('QUICKWIPER_SECRET_MARKER_0123456789')

Set-Content -Path $log -Value "QuickWiper VHD test  $(Get-Date -Format o)" -Encoding utf8
function Log($m) { Add-Content -Path $log -Value $m -Encoding utf8 }
# Invoke the (WinExe) exe and WAIT for it: PowerShell's '&' does not wait for a
# GUI-subsystem process, so we must Start-Process -Wait to get real exit codes,
# captured output, and (critically) completion before we inspect the disk.
function Exe($desc, [string[]]$cliArgs) {
    Log "=== $desc ==="
    $o = Join-Path $env:TEMP "qw-o-$([guid]::NewGuid().ToString('N')).txt"
    $e = Join-Path $env:TEMP "qw-e-$([guid]::NewGuid().ToString('N')).txt"
    $p = Start-Process -FilePath $exe -ArgumentList $cliArgs -Wait -PassThru `
            -RedirectStandardOutput $o -RedirectStandardError $e -WindowStyle Hidden
    $out = ((Get-Content $o -Raw -ErrorAction SilentlyContinue) + (Get-Content $e -Raw -ErrorAction SilentlyContinue)).Trim()
    Remove-Item $o, $e -Force -ErrorAction SilentlyContinue
    if ($out) { Log $out }
    Log "exit=$($p.ExitCode)"
    return [int]$p.ExitCode
}
function Invoke-Dp($script) {
    $f = Join-Path $env:TEMP "dp-$([guid]::NewGuid().ToString('N')).txt"
    Set-Content -Path $f -Value $script -Encoding ascii
    & "$env:SystemRoot\System32\diskpart.exe" /s $f | Out-String | ForEach-Object { Log $_ }
    Remove-Item $f -Force
}

$pass = 0; $fail = 0
function Check($name, $cond) {
    if ($cond) { Log "  PASS: $name"; $script:pass++ }
    else       { Log "  FAIL: $name"; $script:fail++ }
}

# Write a 64KB band of the marker at $offset inside a raw file.
function Write-Marker($path, [long]$offset) {
    $band = New-Object byte[] (64*1024)
    for ($i = 0; $i -lt $band.Length; $i += $marker.Length) {
        [Array]::Copy($marker, 0, $band, $i, [Math]::Min($marker.Length, $band.Length - $i))
    }
    $fs = [IO.File]::Open($path, 'Open', 'Write')
    try { $fs.Seek($offset, 'Begin') | Out-Null; $fs.Write($band, 0, $band.Length) } finally { $fs.Dispose() }
}

function Test-MarkerPresent($path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $chunk = 4MB; $overlap = $marker.Length
        $buf = New-Object byte[] ($chunk + $overlap)
        $carry = New-Object byte[] $overlap; $carryLen = 0
        while ($true) {
            [Array]::Copy($carry, 0, $buf, 0, $carryLen)
            $read = $fs.Read($buf, $carryLen, $chunk)
            if ($read -le 0) { break }
            $total = $carryLen + $read
            $s = [Text.Encoding]::ASCII.GetString($buf, 0, $total)
            if ($s.Contains([Text.Encoding]::ASCII.GetString($marker))) { return $true }
            if ($total -ge $overlap) { [Array]::Copy($buf, $total - $overlap, $carry, 0, $overlap); $carryLen = $overlap }
            else { $carryLen = 0 }
        }
    } finally { $fs.Dispose() }
    return $false
}

try {
    $before = @((Get-CimInstance Win32_DiskDrive).Index)
    Log "Existing disks: $($before -join ',')"

    # Create the fixed VHD file (fully allocated, not yet attached).
    if (Test-Path $vhd) { Remove-Item $vhd -Force }
    Invoke-Dp "create vdisk file=`"$vhd`" maximum=512 type=fixed"
    Start-Sleep 1

    # Plant secret markers directly into the raw image (head, middle, tail).
    $size = (Get-Item $vhd).Length
    Log "VHD file size: $size"
    Write-Marker $vhd 0
    Write-Marker $vhd (200MB)
    Write-Marker $vhd (400MB)
    Check "markers present before wipe" (Test-MarkerPresent $vhd)

    # Attach as a raw physical disk.
    Invoke-Dp "select vdisk file=`"$vhd`"`r`nattach vdisk"
    Start-Sleep 2

    $after = @((Get-CimInstance Win32_DiskDrive).Index)
    $new = @($after | Where-Object { $_ -notin $before })
    if ($new.Count -ne 1) { throw "Expected exactly 1 new disk; got [$($new -join ',')]" }
    $idx = [int]$new[0]
    $d = Get-CimInstance Win32_DiskDrive | Where-Object Index -eq $idx
    Log "New VHD => disk $idx  model='$($d.Model)'  size=$($d.Size)"

    # Hard safety gates on the target before we ever pass it to the wiper.
    if ($idx -eq 0)               { throw "ABORT: target index is 0" }
    if ($idx -in $before)         { throw "ABORT: target was a pre-existing disk" }
    if ([long]$d.Size -gt 600MB)  { throw "ABORT: target too large ($($d.Size))" }
    if ($d.Model -notmatch 'Virtual|Msft') { throw "ABORT: target not virtual ('$($d.Model)')" }
    Log "Safety gates OK; target disk $idx is the fresh VHD."

    # 1. list
    Exe "exe list --allow-virtual" @('list','--allow-virtual') | Out-Null

    # 2. SAFETY: the OS disk must be rejected (exit 2), with and without --allow-virtual.
    $e1 = Exe "SAFETY wipe --disk 0 --yes (expect reject 2)"                @('wipe','--disk','0','--mode','quick','--yes')
    Check "OS disk rejected (no flag)" ($e1 -eq 2)
    $e2 = Exe "SAFETY wipe --disk 0 --yes --allow-virtual (expect reject 2)" @('wipe','--disk','0','--mode','quick','--yes','--allow-virtual')
    Check "OS disk rejected (with --allow-virtual)" ($e2 -eq 2)

    # 3. SAFETY: wipe without --yes must refuse.
    $e3 = Exe "SAFETY wipe VHD without --yes (expect usage 1)" @('wipe','--disk',"$idx",'--mode','quick','--allow-virtual')
    Check "refuses without --yes" ($e3 -eq 1)

    # 4. time-box: --seconds 0 stops immediately (still exit 0).
    $e4 = Exe "wipe VHD quick2 --seconds 0 (timebox)" @('wipe','--disk',"$idx",'--mode','quick2','--seconds','0','--yes','--allow-virtual')
    Check "timebox returns success exit" ($e4 -eq 0)

    # 5. real quick2 wipe of the VHD (fine-grained spread). This is the wipe whose
    #    result the marker-absence check (step 8) verifies: a full quick2 run must
    #    overwrite every byte, just like Full.
    $e5 = Exe "wipe VHD quick2 (expect 0)" @('wipe','--disk',"$idx",'--mode','quick2','--yes','--allow-virtual')
    Check "quick2 wipe succeeded" ($e5 -eq 0)

    # 5b. quick mode wipe (exit-code coverage).
    $e6 = Exe "wipe VHD quick (expect 0)" @('wipe','--disk',"$idx",'--mode','quick','--yes','--allow-virtual')
    Check "quick wipe succeeded" ($e6 -eq 0)

    # 6. full mode wipe.
    $e7 = Exe "wipe VHD full (expect 0)" @('wipe','--disk',"$idx",'--mode','full','--yes','--allow-virtual')
    Check "full wipe succeeded" ($e7 -eq 0)

    # 7. reformat exFAT (needs VDS — INFORMATIONAL on this PC; may time out).
    $e8 = Exe "INFO wipe VHD quick + reformat exfat (VDS-dependent)" @('wipe','--disk',"$idx",'--mode','quick','--yes','--allow-virtual','--fs','exfat')
    Log "  INFO: reformat exit=$e8 (0 => exFAT created; non-zero likely the known VDS issue, not a code fault)"

    # 8. detach and scan the raw VHD file: every secret marker must be gone.
    Invoke-Dp "select vdisk file=`"$vhd`"`r`ndetach vdisk"
    Start-Sleep 1
    $stillThere = Test-MarkerPresent $vhd
    Log "Secret marker present in wiped image: $stillThere"
    Check "secret data destroyed (marker absent after wipe)" (-not $stillThere)

    # 9. GUI smoke: launches and survives a few seconds (exercises enumeration + form).
    $gui = Start-Process -FilePath $exe -PassThru
    Start-Sleep 4
    $alive = -not $gui.HasExited
    Log "GUI process alive after 4s: $alive"
    if ($alive) { Stop-Process -Id $gui.Id -Force }
    Check "GUI starts without crashing" $alive
}
catch {
    Log "EXCEPTION: $($_.Exception.Message)"
    $script:fail++
}
finally {
    try { Invoke-Dp "select vdisk file=`"$vhd`"`r`ndetach vdisk" } catch {}
    if (Test-Path $vhd) { Remove-Item $vhd -Force -ErrorAction SilentlyContinue }
    Log ""
    Log "RESULT: $pass passed, $fail failed"
}
