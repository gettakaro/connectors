param(
  [string]$DevKitRoot = $env:TAKARO_CONAN_DEVKIT_ROOT,
  [string]$ModName = "TakaroConan",
  [int64]$RequiredFreeBytes = 322122547200
)

$ErrorActionPreference = "Stop"
$failures = 0

function Ok($message) {
  Write-Host "OK: $message"
}

function Fail($message) {
  Write-Host "FAIL: $message" -ForegroundColor Red
  $script:failures += 1
}

function Test-FileAny($label, [string[]]$paths) {
  foreach ($path in $paths) {
    if ($path -and (Test-Path -LiteralPath $path)) {
      Ok "$label found: $path"
      return $path
    }
  }
  Fail "$label missing"
  return $null
}

if (-not $DevKitRoot) {
  $candidates = @(
    "C:\ConanExilesDevKit",
    "C:\ConanExilesEnhancedDevKit",
    "C:\Program Files\Epic Games\ConanExilesEnhancedDevKit",
    "C:\Program Files\Epic Games\Conan Exiles Enhanced Dev Kit",
    "D:\ConanExilesDevKit",
    "D:\ConanExilesEnhancedDevKit",
    "D:\Epic Games\ConanExilesEnhancedDevKit",
    "D:\Epic Games\Conan Exiles Enhanced Dev Kit"
  )
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) {
      $DevKitRoot = $candidate
      break
    }
  }
}

if ($DevKitRoot -and (Test-Path -LiteralPath $DevKitRoot)) {
  Ok "DevKit root: $DevKitRoot"
} else {
  Fail "DevKit root missing; pass -DevKitRoot or set TAKARO_CONAN_DEVKIT_ROOT"
}

if ($DevKitRoot) {
  Test-FileAny "Unreal editor" @(
    (Join-Path $DevKitRoot "Engine\Binaries\Win64\UnrealEditor.exe"),
    (Join-Path $DevKitRoot "Engine\Binaries\Win64\UE4Editor.exe")
  ) | Out-Null

  Test-FileAny "RunUAT" @(
    (Join-Path $DevKitRoot "Engine\Build\BatchFiles\RunUAT.bat")
  ) | Out-Null

  Test-FileAny "UnrealPak" @(
    (Join-Path $DevKitRoot "Engine\Binaries\Win64\UnrealPak.exe")
  ) | Out-Null

  $modsRoot = Join-Path $DevKitRoot "Games\ConanSandbox\Content\Mods"
  $activeModRoot = Join-Path $modsRoot $ModName
  if (Test-Path -LiteralPath $activeModRoot) {
    Ok "active DevKit mod folder exists: $activeModRoot"
    $assets = Get-ChildItem -LiteralPath $activeModRoot -Recurse -Include *.uasset,*.umap -ErrorAction SilentlyContinue
    if ($assets.Count -gt 0) {
      Ok "DevKit mod assets found: $($assets.Count)"
    } else {
      Fail "DevKit mod folder exists but contains no .uasset/.umap files"
    }

    $modController = Get-ChildItem -LiteralPath $activeModRoot -Recurse -Filter "TakaroConan_ModController*.uasset" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($modController) {
      Ok "TakaroConan_ModController asset found: $($modController.FullName)"
    } else {
      Fail "TakaroConan_ModController asset missing under active DevKit mod folder"
    }

    $chatCommand = Get-ChildItem -LiteralPath $activeModRoot -Recurse -Filter "BP_TakaroChatCommand*.uasset" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($chatCommand) {
      Ok "BP_TakaroChatCommand asset found: $($chatCommand.FullName)"
    } else {
      Fail "BP_TakaroChatCommand asset missing under active DevKit mod folder"
    }

    $commandTable = Get-ChildItem -LiteralPath $activeModRoot -Recurse -Filter "DT_TakaroConsoleCommands*.uasset" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($commandTable) {
      Ok "DT_TakaroConsoleCommands asset found: $($commandTable.FullName)"
    } else {
      Fail "DT_TakaroConsoleCommands asset missing under active DevKit mod folder"
    }
  } else {
    Fail "active DevKit mod folder missing: $activeModRoot"
  }
}

$driveRoot = if ($DevKitRoot) { [System.IO.Path]::GetPathRoot($DevKitRoot) } else { "C:\" }
try {
  $drive = Get-PSDrive -Name $driveRoot.Substring(0, 1)
  if ($drive.Free -ge $RequiredFreeBytes) {
    Ok "free disk satisfies threshold on $driveRoot: $($drive.Free) bytes"
  } else {
    Fail "free disk below threshold on $driveRoot: $($drive.Free) bytes available, $RequiredFreeBytes required"
  }
} catch {
  Fail "could not inspect free disk for $driveRoot: $($_.Exception.Message)"
}

if ($failures -gt 0) {
  Write-Host ""
  Write-Host "TakaroConan Windows DevKit preflight failed ($failures failure(s))." -ForegroundColor Red
  exit 1
}

Write-Host ""
Write-Host "TakaroConan Windows DevKit preflight passed."
