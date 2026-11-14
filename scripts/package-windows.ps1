param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputZip
)

$ErrorActionPreference = 'Stop'
if ([IO.Path]::GetExtension($OutputZip) -ne '.zip') {
    throw 'The output path must end in .zip.'
}
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputZip)
if (Test-Path -LiteralPath $output) {
    throw "Output already exists: $output"
}

$outputDirectory = Split-Path -Parent $output
$null = New-Item -ItemType Directory -Path $outputDirectory -Force
$work = Join-Path $outputDirectory ('.frameline-package-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'Frameline'
$null = New-Item -ItemType Directory -Path $stage
try {
    cmake --install $build --config Release --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw 'CMake install failed.' }

    if (!$env:VCToolsRedistDir) { throw 'Run packaging in the x64 Visual C++ developer shell.' }
    $runtimeRoot = Join-Path $env:VCToolsRedistDir 'x64'
    $runtime = @(Get-ChildItem -LiteralPath $runtimeRoot -Directory -Filter 'Microsoft.VC*.CRT')
    if ($runtime.Count -ne 1) { throw "Expected one redistributable x64 runtime in $runtimeRoot." }
    $bin = Join-Path $stage 'bin'
    Get-ChildItem -LiteralPath $runtime[0].FullName -File -Filter '*.dll' | Copy-Item -Destination $bin

    $requiredFiles = @(
        'bin/frameline.exe', 'bin/FramelineGUI.exe', 'bin/qt.conf',
        'bin/Qt6Core.dll', 'bin/Qt6Gui.dll', 'bin/Qt6Network.dll', 'bin/Qt6Widgets.dll',
        'bin/vcruntime140.dll', 'bin/msvcp140.dll',
        'plugins/platforms/qwindows.dll', 'plugins/imageformats/qjpeg.dll',
        'LICENSE', 'RELEASE.md', 'Qt-notices/README.txt'
    )
    foreach ($file in $requiredFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $stage $file) -PathType Leaf)) {
            throw "Deployment is missing $file. Use the release Qt SDK and the Visual C++ build environment."
        }
    }

    $cli = Join-Path $stage 'bin/frameline.exe'
    & $cli --version
    if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI did not start.' }

    $archive = Join-Path $work 'Frameline.zip'
    Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
    [IO.File]::Move($archive, $output)
    Write-Output "Created $output"
}
finally {
    $resolvedWork = (Resolve-Path -LiteralPath $work).Path
    if ((Split-Path -Parent $resolvedWork) -eq $outputDirectory) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
