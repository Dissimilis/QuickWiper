# Scan a raw physical disk for a marker string. Read-only.
# Usage: raw-scan.ps1 -Disk 1 -Marker '...' -MaxBytes 3GB
param(
    [Parameter(Mandatory)] [int]$Disk,
    [Parameter(Mandatory)] [string]$Marker,
    [long]$MaxBytes = 3GB
)
$ErrorActionPreference = 'Stop'
$markerBytes = [Text.Encoding]::ASCII.GetBytes($Marker)
$path = "\\.\PhysicalDrive$Disk"
$h = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    $chunk = 8MB
    $overlap = $markerBytes.Length
    $buf = New-Object byte[] ($chunk + $overlap)
    $carry = New-Object byte[] $overlap; $carryLen = 0
    $scanned = 0L
    $markerStr = [Text.Encoding]::ASCII.GetString($markerBytes)
    while ($scanned -lt $MaxBytes) {
        [Array]::Copy($carry, 0, $buf, 0, $carryLen)
        $read = $h.Read($buf, $carryLen, $chunk)
        if ($read -le 0) { break }
        $total = $carryLen + $read
        $s = [Text.Encoding]::ASCII.GetString($buf, 0, $total)
        $hit = $s.IndexOf($markerStr)
        if ($hit -ge 0) {
            [PSCustomObject]@{ Found = $true; OffsetApprox = $scanned - $carryLen + $hit; Scanned = $scanned + $read }
            return
        }
        if ($total -ge $overlap) { [Array]::Copy($buf, $total - $overlap, $carry, 0, $overlap); $carryLen = $overlap }
        else { $carryLen = 0 }
        $scanned += $read
    }
    [PSCustomObject]@{ Found = $false; OffsetApprox = -1; Scanned = $scanned }
} finally { $h.Dispose() }
