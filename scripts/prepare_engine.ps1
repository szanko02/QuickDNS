param([Parameter(Mandatory)][string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$version = '1.14.4'
$name = "sing-box-$version-windows-amd64"
$sha256 = '99185d75d314a07dbba4a130c8ad986cbeb629a07927975636b341ca61f991df'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$archive = Join-Path $OutputDirectory "$name.zip"
function Get-ArchiveDigest {
    $stream = [IO.File]::OpenRead($archive)
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '')
    } finally {
        $hash.Dispose()
        $stream.Dispose()
    }
}
if (!(Test-Path -LiteralPath $archive) -or (Get-ArchiveDigest) -ne $sha256) {
    Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/SagerNet/sing-box/releases/download/v$version/$name.zip" -OutFile $archive
}
if ((Get-ArchiveDigest) -ne $sha256) {
    throw 'The routing engine archive failed SHA-256 verification.'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entry = $zip.GetEntry("$name/sing-box.exe")
    if (!$entry) { throw 'The routing engine executable is missing from its archive.' }
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $OutputDirectory 'sing-box.exe'), $true)
} finally {
    $zip.Dispose()
}

$resourcePath = (Join-Path $OutputDirectory 'sing-box.exe').Replace('\', '/')
Set-Content -LiteralPath (Join-Path $OutputDirectory 'embedded_engine.rc') -Encoding Unicode -Value "4301 RCDATA `"$resourcePath`""
