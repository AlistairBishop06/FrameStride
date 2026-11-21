[CmdletBinding()]
param(
    [string]$Archive,
    [string]$Target,
    [ValidatePattern('^[a-zA-Z_][a-zA-Z0-9_-]*$')][string]$User = 'steamos'
)
$ErrorActionPreference = 'Stop'
if (!$Archive) {
    $name = 'framestride-1.0.0-linux-aarch64.tar.gz'
    $Archive = Join-Path $PSScriptRoot $name
    if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) {
        $Archive = Join-Path (Split-Path $PSScriptRoot -Parent) $name
    }
}
if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) { throw 'Pass the downloaded .tar.gz using -Archive.' }
$Archive = (Resolve-Path -LiteralPath $Archive).Path
$ssh = (Get-Command ssh.exe -CommandType Application).Source
$scp = (Get-Command scp.exe -CommandType Application).Source
if (!$Target) {
    try { $null = [System.Net.Dns]::GetHostAddresses('frame'); $Target = 'frame' }
    catch { $Target = Read-Host 'Steam Frame IPv4 address or hostname' }
}
if ($Target -notmatch '^[a-zA-Z0-9][a-zA-Z0-9.-]*$') { throw 'Enter an IPv4 address or hostname.' }
$device = "$User@$Target"
$connection = @('-o', 'ConnectTimeout=10')
$remote = & $ssh @connection $device 'mktemp -d /tmp/framestride.XXXXXXXX'
if ($LASTEXITCODE -ne 0 -or $remote -isnot [string] -or $remote -notmatch '^/tmp/framestride\.[a-zA-Z0-9]{8}$') {
    throw 'SSH did not return a staging directory. Check Developer Mode and SSH access.'
}
try {
    & $scp @connection -- $Archive "${device}:$remote/release.tar.gz"
    if ($LASTEXITCODE -ne 0) { throw 'Could not transfer the release archive.' }
    & $ssh @connection $device "cd '$remote' && tar -xzf release.tar.gz --strip-components=1 && bash ./install.sh"
    if ($LASTEXITCODE -ne 0) { throw 'The Steam Frame installer returned an error.' }
} finally {
    & $ssh @connection $device "rm -rf -- '$remote'"
}
