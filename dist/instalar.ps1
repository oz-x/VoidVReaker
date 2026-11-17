# Instalador interactivo de VoidVReaker (VOID/BREAKER en realidad virtual).
# Busca VOID/BREAKER (Steam y Epic), pide confirmacion y copia el mod en VOIDBREAKER\Binaries\Win64.
#
#   powershell -ExecutionPolicy Bypass -File instalar.ps1
#   powershell -ExecutionPolicy Bypass -File instalar.ps1 -GamePath "D:\Juegos\VOIDBREAKER" -Yes
#
# -GamePath: carpeta del juego (o su Binaries\Win64) sin buscarla.  -Yes: no preguntar nada.
param(
    [string]$GamePath,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
$SteamAppId = '2615540'
$ModFiles = @('version.dll', 'openxr_loader.dll')
$GameExe = 'VOIDBREAKER-Win64-Shipping.exe'
$Source = $PSScriptRoot

function Write-Title([string]$text) {
    Write-Host ''
    Write-Host $text -ForegroundColor Cyan
}

function Finish([int]$code) {
    if (-not $Yes) {
        Write-Host ''
        Read-Host 'Pulsa Enter para salir' | Out-Null
    }
    exit $code
}

function Fail([string]$message) {
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    Finish 1
}

function Ask-YesNo([string]$question, [bool]$default) {
    if ($Yes) { return $true }
    $hint = if ($default) { '[S/n]' } else { '[s/N]' }
    while ($true) {
        $answer = (Read-Host "$question $hint").Trim().ToLower()
        if ($answer -eq '') { return $default }
        if ($answer -in @('s', 'si', 'y', 'yes')) { return $true }
        if ($answer -in @('n', 'no')) { return $false }
        Write-Host 'Responde s o n.'
    }
}

# Accepts the game folder, its Binaries\Win64 folder or the game's executable and returns the
# folder of VOIDBREAKER-Win64-Shipping.exe, or $null if it is not a VOID/BREAKER installation.
function Resolve-GameBinaries([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    $path = $path.Trim().Trim('"')
    if (Test-Path -LiteralPath $path -PathType Leaf) { $path = Split-Path -Parent $path }
    foreach ($candidate in @($path, (Join-Path $path 'Binaries\Win64'), (Join-Path $path 'VOIDBREAKER\Binaries\Win64'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate $GameExe) -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }
    return $null
}

function Get-GameRoot([string]$binaries) {
    return Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $binaries))
}

function Get-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($key in @(
            @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
            @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
            @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' })) {
        $value = (Get-ItemProperty -Path $key.Path -Name $key.Name -ErrorAction SilentlyContinue).($key.Name)
        if ($value) { $roots.Add(($value -replace '/', '\')) }
    }
    $libraries = New-Object System.Collections.Generic.List[string]
    foreach ($root in $roots) {
        $libraries.Add($root)
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries.Add(($match.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
    return $libraries | Where-Object { Test-Path -LiteralPath $_ } | Sort-Object -Unique
}

function Find-Installations {
    $found = New-Object System.Collections.Generic.List[object]
    foreach ($library in Get-SteamLibraries) {
        $folder = 'VOIDBREAKER'
        $manifest = Join-Path $library "steamapps\appmanifest_$SteamAppId.acf"
        if (Test-Path -LiteralPath $manifest) {
            $match = [regex]::Match((Get-Content -LiteralPath $manifest -Raw), '"installdir"\s+"([^"]+)"')
            if ($match.Success) { $folder = $match.Groups[1].Value }
        }
        $binaries = Resolve-GameBinaries (Join-Path $library "steamapps\common\$folder")
        if ($binaries) { $found.Add([pscustomobject]@{ Store = 'Steam'; Binaries = $binaries }) }
    }
    $epic = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $epic) {
        foreach ($item in Get-ChildItem -LiteralPath $epic -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $data = Get-Content -LiteralPath $item.FullName -Raw | ConvertFrom-Json } catch { continue }
            if ($data.DisplayName -notlike '*VOID*BREAKER*') { continue }
            $binaries = Resolve-GameBinaries $data.InstallLocation
            if ($binaries) { $found.Add([pscustomobject]@{ Store = 'Epic'; Binaries = $binaries }) }
        }
    }
    # The same folder can be reached through several registry keys / library entries.
    return @($found | Sort-Object Binaries -Unique)
}

function Select-Installation {
    if ($GamePath) {
        $binaries = Resolve-GameBinaries $GamePath
        if (-not $binaries) { Fail "No encuentro $GameExe en '$GamePath'." }
        return $binaries
    }
    Write-Title 'Buscando VOID/BREAKER...'
    $found = @(Find-Installations)  # @(): a single result would otherwise lose .Count
    if ($found.Count -eq 1) {
        Write-Host "Encontrado ($($found[0].Store)): $(Get-GameRoot $found[0].Binaries)"
        if (Ask-YesNo 'Es esta la instalacion correcta?' $true) { return $found[0].Binaries }
    } elseif ($found.Count -gt 1) {
        Write-Host 'He encontrado varias instalaciones:'
        for ($i = 0; $i -lt $found.Count; $i++) {
            $root = Get-GameRoot $found[$i].Binaries
            Write-Host ("  [{0}] {1} ({2})" -f ($i + 1), $root, $found[$i].Store)
        }
        if ($Yes) { return $found[0].Binaries }
        while ($true) {
            $choice = Read-Host "Elige un numero (1-$($found.Count)), o Enter para escribir otra ruta"
            if ($choice -eq '') { break }
            $index = 0
            if ([int]::TryParse($choice, [ref]$index) -and $index -ge 1 -and $index -le $found.Count) {
                return $found[$index - 1].Binaries
            }
        }
    } else {
        Write-Host 'No he encontrado VOID/BREAKER automaticamente.'
    }
    if ($Yes) { Fail 'No se ha encontrado el juego; indica la ruta con -GamePath.' }
    while ($true) {
        $typed = Read-Host 'Ruta de la carpeta de VOID/BREAKER (vacio para cancelar)'
        if ([string]::IsNullOrWhiteSpace($typed)) { Write-Host 'Cancelado.'; Finish 1 }
        $binaries = Resolve-GameBinaries $typed
        if ($binaries) { return $binaries }
        Write-Host "No encuentro $GameExe ahi (ni en VOIDBREAKER\Binaries\Win64)." -ForegroundColor Yellow
    }
}

Write-Host '=============================================' -ForegroundColor Cyan
Write-Host '     VoidVReaker - instalador' -ForegroundColor Cyan
Write-Host '=============================================' -ForegroundColor Cyan
Write-Host ''
Write-Host 'AVISO: el mod esta en FASE EXPERIMENTAL. Puede tener fallos, cierres o' -ForegroundColor Yellow
Write-Host 'cosas sin terminar. Para quitarlo, borra los archivos que copia (ver LEEME.md).' -ForegroundColor Yellow

foreach ($file in $ModFiles + 'voidvr.ini') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $file))) {
        Fail "Falta '$file' junto al instalador. Descomprime el ZIP completo antes de ejecutarlo."
    }
}

$target = Select-Installation
$existingDll = Join-Path $target 'version.dll'
$existingIni = Join-Path $target 'voidvr.ini'
# Our DLL always has a voidvr.ini next to it; a lone version.dll belongs to another mod.
$update = (Test-Path -LiteralPath $existingDll) -and (Test-Path -LiteralPath $existingIni)
$foreign = (Test-Path -LiteralPath $existingDll) -and -not $update
$backup = Join-Path $target ('version.dll.bak-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))

Write-Title 'Resumen'
Write-Host "  Destino:  $target"
Write-Host "  Accion:   $(if ($update) { 'actualizar el mod ya instalado' } else { 'instalar el mod' })"
Write-Host "  Archivos: $($ModFiles -join ', ')"
if (Test-Path -LiteralPath $existingIni) {
    Write-Host '  Config:   se conserva tu voidvr.ini actual'
} else {
    Write-Host '  Config:   voidvr.ini con los valores por defecto'
}
if ($foreign) {
    Write-Host "  Aviso:    ya hay otro version.dll (otro mod); se guardara como $(Split-Path -Leaf $backup)" -ForegroundColor Yellow
}
Write-Host ''
if (-not (Ask-YesNo 'Instalar ahora?' $true)) {
    Write-Host 'Cancelado; no se ha cambiado nada.'
    Finish 1
}

$running = Get-Process -Name 'VOIDBREAKER-Win64-Shipping' -ErrorAction SilentlyContinue
if ($running) {
    if (-not (Ask-YesNo 'VOID/BREAKER esta abierto y hay que cerrarlo. Cerrarlo ahora?' $false)) {
        Fail 'Cierra VOID/BREAKER y vuelve a ejecutar el instalador.'
    }
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}

try {
    if ($foreign) { Move-Item -LiteralPath $existingDll -Destination $backup }
    foreach ($file in $ModFiles) {
        Copy-Item -LiteralPath (Join-Path $Source $file) -Destination $target -Force
    }
    if (-not (Test-Path -LiteralPath $existingIni)) {
        Copy-Item -LiteralPath (Join-Path $Source 'voidvr.ini') -Destination $target
    }
} catch {
    Fail "No se pudieron copiar los archivos: $($_.Exception.Message)"
}

Write-Title 'Instalado.'
Write-Host '  1. Abre Virtual Desktop / Quest Link / SteamVR y conecta el visor.'
Write-Host '  2. Lanza VOID/BREAKER desde Steam como siempre.'
Write-Host "  3. Ajustes en: $existingIni"
Write-Host '  Para desinstalar, borra version.dll, openxr_loader.dll y voidvr.ini de esa carpeta.'
Finish 0
