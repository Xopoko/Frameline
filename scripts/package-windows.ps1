[CmdletBinding(DefaultParameterSetName = 'Zip')]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory, ParameterSetName = 'Zip')][string]$OutputZip,
    [Parameter(Mandatory, ParameterSetName = 'Store')][string]$OutputMsix,
    [Parameter(Mandatory, ParameterSetName = 'Store')]
    [ValidatePattern('^[a-zA-Z0-9.-]{3,50}$')][string]$IdentityName,
    [Parameter(Mandatory, ParameterSetName = 'Store')][string]$Publisher,
    [Parameter(Mandatory, ParameterSetName = 'Store')][string]$PublisherDisplayName,
    [Parameter(Mandatory, ParameterSetName = 'Store')]
    [ValidatePattern('^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$')][string]$PackageVersion,
    [Parameter(ParameterSetName = 'Store')]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$WindowsSdkVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$store = $PSCmdlet.ParameterSetName -eq 'Store'
if ($store) {
    if ([IO.Path]::GetExtension($OutputMsix) -ne '.msix') {
        throw 'The output path must end in .msix.'
    }
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
    if ($cache -notcontains 'FREEZECUT_WINDOWS_STORE:BOOL=ON') {
        throw 'Configure this build with FREEZECUT_WINDOWS_STORE=ON.'
    }
    $version = [version]$PackageVersion
    if ($version.Major -gt 65535 -or $version.Minor -gt 65535 -or $version.Build -gt 65535) {
        throw 'MSIX version components must be at most 65535.'
    }
    $makeappx = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin/$WindowsSdkVersion/x64/makeappx.exe"
    if (!(Test-Path -LiteralPath $makeappx -PathType Leaf)) {
        throw "MakeAppx.exe was not found for Windows SDK $WindowsSdkVersion."
    }
    $outputPath = $OutputMsix
} else {
    if ([IO.Path]::GetExtension($OutputZip) -ne '.zip') {
        throw 'The output path must end in .zip.'
    }
    $outputPath = $OutputZip
}
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($outputPath)
if (Test-Path -LiteralPath $output) {
    throw "Output already exists: $output"
}

$outputDirectory = Split-Path -Parent $output
$null = New-Item -ItemType Directory -Path $outputDirectory -Force
$work = Join-Path $outputDirectory ('.freezecut-package-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'FreezeCut'
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
        'bin/freezecut.exe', 'bin/FreezeCutGUI.exe', 'bin/qt.conf',
        'bin/frameline.exe', 'bin/FramelineGUI.exe',
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

    $cli = Join-Path $stage 'bin/freezecut.exe'
    & $cli --version
    if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI did not start.' }

    if ($store) {
        $gui = Join-Path $bin 'FreezeCutGUI.exe'
        $pe = [IO.BinaryReader]::new([IO.File]::OpenRead($gui))
        try {
            $pe.BaseStream.Position = 0x3C
            $pe.BaseStream.Position = $pe.ReadInt32()
            if ($pe.ReadUInt32() -ne 0x4550 -or $pe.ReadUInt16() -ne 0x8664) {
                throw 'The MSIX requires an x64 Windows build.'
            }
        } finally {
            $pe.Dispose()
        }

        $resources = Join-Path (Split-Path -Parent $PSScriptRoot) 'resources'
        [xml]$manifest = Get-Content -LiteralPath (Join-Path $resources 'FreezeCutStore.appxmanifest') -Raw
        $namespaces = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
        $namespaces.AddNamespace('appx', $manifest.DocumentElement.NamespaceURI)
        $identity = $manifest.SelectSingleNode('/appx:Package/appx:Identity', $namespaces)
        $identity.SetAttribute('Name', $IdentityName)
        $identity.SetAttribute('Publisher', $Publisher)
        $identity.SetAttribute('Version', $PackageVersion)
        $manifest.SelectSingleNode('/appx:Package/appx:Properties/appx:PublisherDisplayName',
            $namespaces).InnerText = $PublisherDisplayName
        $manifest.Save((Join-Path $stage 'AppxManifest.xml'))

        Add-Type -AssemblyName System.Drawing
        $logos = @{ 'StoreLogo.png' = 50; 'Square44x44Logo.png' = 44; 'Square150x150Logo.png' = 150 }
        $assets = Join-Path $stage 'Assets'
        $null = New-Item -ItemType Directory -Path $assets
        $icon = [Drawing.Image]::FromFile((Join-Path $resources 'FreezeCut.png'))
        try {
            foreach ($logo in $logos.GetEnumerator()) {
                $image = [Drawing.Bitmap]::new($logo.Value, $logo.Value)
                $graphics = [Drawing.Graphics]::FromImage($image)
                try {
                    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.DrawImage($icon, 0, 0, $image.Width, $image.Height)
                    $image.Save((Join-Path $assets $logo.Key), [Drawing.Imaging.ImageFormat]::Png)
                } finally {
                    $graphics.Dispose()
                    $image.Dispose()
                }
            }
        } finally {
            $icon.Dispose()
        }

        $archive = Join-Path $work 'FreezeCut.msix'
        & $makeappx pack /d $stage /p $archive
        if ($LASTEXITCODE -ne 0) { throw 'MakeAppx packaging failed.' }
    } else {
        $archive = Join-Path $work 'FreezeCut.zip'
        Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
    }
    [IO.File]::Move($archive, $output)
    Write-Output "Created $output"
}
finally {
    $resolvedWork = (Resolve-Path -LiteralPath $work).Path
    if ((Split-Path -Parent $resolvedWork) -eq $outputDirectory) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
