[CmdletBinding(SupportsShouldProcess)]
param([string]$Cs2Root)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-Cs2Folder([string]$Path) {
    return (Test-Path -LiteralPath (Join-Path $Path 'game/bin/win64/cs2.exe') -PathType Leaf)
}

try {
    $files = @(
        'content/csgo_addons/routeline/addoninfo.txt'
        'content/csgo_addons/routeline/particles/routeline/route.vpcf'
        'content/csgo_addons/routeline/particles/routeline/marker.vpcf'
        'content/csgo_addons/routeline/particles/routeline/marker_flat.vpcf'
        'content/csgo_addons/routeline/particles/routeline/marker_flat_blue.vpcf'
        'content/csgo_addons/routeline/particles/routeline/marker_flat_red.vpcf'
        'content/csgo_addons/routeline/materials/routeline/marker_flat.vtex'
        'content/csgo_addons/routeline/materials/routeline/marker_flat.png'
        'content/csgo_addons/routeline/particles/routeline/jump_flat_amber.vpcf'
        'content/csgo_addons/routeline/particles/routeline/jump_flat_green.vpcf'
        'content/csgo_addons/routeline/materials/routeline/jump_flat.vtex'
        'content/csgo_addons/routeline/materials/routeline/jump_flat.png'
        'content/csgo_addons/routeline/materials/routeline/marker_mesh.vmat'
        'content/csgo_addons/routeline/models/routeline/landing_mark.vmdl'
        'content/csgo_addons/routeline/models/routeline/landing_mark.obj'
        'content/csgo_addons/routeline/models/routeline/jump_mark.vmdl'
        'content/csgo_addons/routeline/models/routeline/jump_mark.obj'
        'game/csgo_addons/routeline/addoninfo.txt'
        'game/csgo_addons/routeline/particles/routeline/route.vpcf_c'
        'game/csgo_addons/routeline/particles/routeline/marker.vpcf_c'
        'game/csgo_addons/routeline/particles/routeline/marker_flat.vpcf_c'
        'game/csgo_addons/routeline/particles/routeline/marker_flat_blue.vpcf_c'
        'game/csgo_addons/routeline/particles/routeline/marker_flat_red.vpcf_c'
        'game/csgo_addons/routeline/materials/routeline/marker_flat.vtex_c'
        'game/csgo_addons/routeline/particles/routeline/jump_flat_amber.vpcf_c'
        'game/csgo_addons/routeline/particles/routeline/jump_flat_green.vpcf_c'
        'game/csgo_addons/routeline/materials/routeline/jump_flat.vtex_c'
        'game/csgo_addons/routeline/materials/routeline/marker_mesh.vmat_c'
        'game/csgo_addons/routeline/materials/routeline/marker_mesh_vmat_g_tcolor_19031fd4.vtex_c'
        'game/csgo_addons/routeline/models/routeline/landing_mark.vmdl_c'
        'game/csgo_addons/routeline/models/routeline/jump_mark.vmdl_c'
    )
    foreach ($relative in $files) {
        if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot $relative) -PathType Leaf)) {
            throw 'Workshop素材ZIPをすべて展開してからバッチを実行してください。'
        }
    }

    if (!$Cs2Root) {
        Write-Host 'SteamでCS2のプロパティを開き、「インストール済みファイル」からフォルダを参照してください。'
        Write-Host 'game と content が入る Counter-Strike Global Offensive フォルダのパスを入力してください。'
        Write-Host '例: D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive'
        $Cs2Root = Read-Host 'CS2のインストール先'
    }
    $Cs2Root = $Cs2Root.Trim().Trim('"')
    if (!$Cs2Root -or !(Test-Cs2Folder $Cs2Root)) {
        throw 'game/bin/win64/cs2.exe があるCS2フォルダを指定してください。'
    }
    $Cs2Root = (Resolve-Path -LiteralPath $Cs2Root).Path
    Write-Host "配置先 $Cs2Root"
    if (!$PSCmdlet.ShouldProcess($Cs2Root, 'RouteLine Workshop素材の配置')) { return }

    $changes = foreach ($relative in $files) {
        $source = Join-Path $PSScriptRoot $relative
        $destination = Join-Path $Cs2Root $relative
        $exists = Test-Path -LiteralPath $destination -PathType Leaf
        if ($exists) {
            # Keep metadata for an addon already configured in Workshop Tools.
            if ($relative.EndsWith('/addoninfo.txt')) { continue }
            if ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) { continue }
        }
        [pscustomobject]@{ Relative = $relative; Source = $source; Destination = $destination; Exists = $exists }
    }

    $backup = Join-Path $PSScriptRoot ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    foreach ($file in $changes) {
        if ($file.Exists) {
            $copy = Join-Path $backup $file.Relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force | Out-Null
            Copy-Item -LiteralPath $file.Destination -Destination $copy
        }
    }
    foreach ($file in $changes) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $file.Destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.Source -Destination $file.Destination -Force
    }

    if (Test-Path -LiteralPath $backup -PathType Container) {
        Write-Host "変更前の素材を保存しました $backup"
    }
    Write-Host 'RouteLine素材の配置が完了しました。' -ForegroundColor Green
    Write-Host 'CS2 Workshop Toolsを開き、Workshop Managerから routeline を選んでください。'
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
