param(
    [Parameter(Mandatory = $true)]
    [string]$Cs2Root,
    [string]$Output = (Join-Path $PSScriptRoot '../build/workshop-compiled')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Cs2Root = (Resolve-Path -LiteralPath $Cs2Root).Path
$game = Join-Path $Cs2Root 'game/csgo'
$compiler = Join-Path $Cs2Root 'game/bin/win64/resourcecompiler.exe'
$content = Join-Path $Cs2Root 'content/csgo_addons/routeline'
$addon = Join-Path $Cs2Root 'game/csgo_addons/routeline'
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'Install Counter-Strike 2 Workshop Tools first.'
}
$assets = @('materials/routeline/marker_flat.vtex', 'particles/routeline/route.vpcf',
    'particles/routeline/marker.vpcf', 'particles/routeline/marker_flat.vpcf',
    'particles/routeline/marker_flat_blue.vpcf', 'particles/routeline/marker_flat_red.vpcf',
    'materials/routeline/jump_flat.vtex', 'particles/routeline/jump_flat_amber.vpcf', 'particles/routeline/jump_flat_green.vpcf',
    'materials/routeline/marker_mesh.vmat', 'models/routeline/landing_mark.vmdl', 'models/routeline/jump_mark.vmdl')
$sourceOnly = @('materials/routeline/marker_flat.png', 'materials/routeline/jump_flat.png',
    'models/routeline/landing_mark.obj', 'models/routeline/jump_mark.obj')
foreach ($asset in ($assets + $sourceOnly)) {
    $target = Join-Path $content $asset
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $asset) -Destination $target -Force
}
foreach ($folder in @($content, $addon, $Output)) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $metadata = Join-Path $folder 'addoninfo.txt'
    if (!(Test-Path -LiteralPath $metadata)) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'addoninfo.txt') -Destination $metadata
    }
}
Push-Location (Split-Path -Parent $compiler)
try {
    foreach ($asset in $assets) {
        & $compiler -game $game -i (Join-Path $content $asset) -f
        if ($LASTEXITCODE) { throw "Workshop resource compiler failed for $asset." }
        $compiled = Join-Path $addon ($asset + '_c')
        if (!(Test-Path -LiteralPath $compiled -PathType Leaf) -or (Get-Item -LiteralPath $compiled).Length -eq 0) {
            throw "Missing compiled effect: $compiled"
        }
        $destination = Join-Path $Output ($asset + '_c')
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $compiled -Destination $destination -Force
    }
} finally {
    Pop-Location
}
# The white constant in marker_mesh.vmat compiles to this texture dependency.
$whiteTexture = 'materials/routeline/marker_mesh_vmat_g_tcolor_19031fd4.vtex_c'
$generated = Join-Path $addon $whiteTexture
if (!(Test-Path -LiteralPath $generated -PathType Leaf) -or (Get-Item -LiteralPath $generated).Length -eq 0) {
    throw "Missing compiled material texture: $generated"
}
Copy-Item -LiteralPath $generated -Destination (Join-Path $Output $whiteTexture) -Force
Write-Host "Compiled RouteLine assets: $addon"
