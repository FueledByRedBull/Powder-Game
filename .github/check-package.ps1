# Fails unless the zip holds exactly the expected files and the executable imports only Windows system DLLs.
param(
    [Parameter(Mandatory)] [string] $Zip,
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string[]] $Expected
)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip))
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName -replace '\\', '/' } | Where-Object { -not $_.EndsWith('/') })
} finally {
    $archive.Dispose()
}
if (Compare-Object -CaseSensitive $entries $Expected) {
    throw "Zip contents differ from the expected files.`nFound: $($entries -join ', ')`nExpected: $($Expected -join ', ')"
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $dumpbin) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $dumpbin = & $vswhere -latest -products * -find 'VC/Tools/MSVC/**/bin/Hostx64/x64/dumpbin.exe' | Select-Object -First 1
}
if (-not $dumpbin) { throw 'dumpbin.exe not found' }
$report = & $dumpbin /nologo /dependents $Exe
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed with exit $LASTEXITCODE" }
$imports = @($report | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
$system = 'KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'SHELL32.dll', 'OPENGL32.dll'
$extra = @($imports | Where-Object { $_ -notin $system })
if ($imports.Count -eq 0 -or $extra.Count -gt 0) {
    throw "Executable imports non-system DLLs: $($extra -join ', ') (all imports: $($imports -join ', '))"
}
"Package OK: $($entries -join ', '); imports $($imports -join ', ')"
