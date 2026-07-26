param(
  [Parameter(Mandatory = $true)][string]$PakPath,
  [string]$OutputDir = ".\TakaroConan-artifact",
  [string]$BuildReportPath = ".\BUILD_REPORT.md",
  [string]$SourceEvidencePath = ".\SOURCE_EVIDENCE.md",
  [Parameter(Mandatory = $true)][string]$DevKitVersion,
  [string]$DevKitBranch = "live / ++exiles+release",
  [string]$DevKitRoot = "<redacted>",
  [string]$SourceRevision = "workspace-snapshot",
  [switch]$CompiledInDevKit,
  [switch]$CookedInDevKit
)

$ErrorActionPreference = "Stop"

function Fail($message) { throw "TakaroConan artifact collection failed: $message" }

if (-not (Test-Path -LiteralPath $PakPath)) { Fail "pak not found: $PakPath" }
if ((Split-Path -Leaf $PakPath) -ne "TakaroConan.pak") { Fail "pak must be named TakaroConan.pak" }
if (-not $CompiledInDevKit) { Fail "pass -CompiledInDevKit after a successful compile" }
if (-not $CookedInDevKit) { Fail "pass -CookedInDevKit after a successful cook" }
if (-not (Test-Path -LiteralPath $BuildReportPath)) { Fail "build report missing" }
if (-not (Test-Path -LiteralPath $SourceEvidencePath)) { Fail "source evidence missing" }

if (Test-Path -LiteralPath $OutputDir) {
  if ((Get-ChildItem -LiteralPath $OutputDir -Force).Count -gt 0) {
    Fail "output directory is not empty; use a fresh directory"
  }
} else {
  New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

$pakHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $PakPath).Hash.ToLowerInvariant()
$pakSize = (Get-Item -LiteralPath $PakPath).Length
$report = Get-Content -LiteralPath $BuildReportPath -Raw
$evidence = Get-Content -LiteralPath $SourceEvidencePath -Raw

foreach ($required in @(
  "TakaroConan_ModController", "BP_TakaroChatCommand", "DT_TakaroConsoleCommands",
  "con <player-index> dc TakaroChat", "W_ChatWindow/W_RichChatLine/FCRichTextBlock",
  "TakaroConan: rendered ", "SOURCE_EVIDENCE.md", "BUILD_SOURCE_CONTRACT.json"
)) {
  if ($report -notmatch [regex]::Escape($required)) { Fail "build report missing: $required" }
}
foreach ($required in @(
  "TakaroConan_ModController", "BP_TakaroChatCommand", "DT_TakaroConsoleCommands",
  "W_ChatWindow", "W_RichChatLine", "FCRichTextBlock", "TakaroConan: rendered ",
  "BUILD_SOURCE_CONTRACT.json version 2"
)) {
  if ($evidence -notmatch [regex]::Escape($required)) { Fail "source evidence missing: $required" }
}
if ($report -match "(?m)- \[ \]") { Fail "build report contains unchecked items" }
if ($evidence -match "(?m)- \[ \]") { Fail "source evidence contains unchecked items" }
if ($report -notmatch [regex]::Escape($pakHash)) { Fail "build report missing exact pak SHA-256" }
if ($report -notmatch "Artifact size:\s*$pakSize(?:\s|$)") { Fail "build report missing exact pak size" }
if ($evidence -notmatch [regex]::Escape($pakHash)) { Fail "source evidence missing exact pak SHA-256" }

Copy-Item -LiteralPath $PakPath -Destination (Join-Path $OutputDir "TakaroConan.pak")
Copy-Item -LiteralPath $BuildReportPath -Destination (Join-Path $OutputDir "BUILD_REPORT.md")
Copy-Item -LiteralPath $SourceEvidencePath -Destination (Join-Path $OutputDir "SOURCE_EVIDENCE.md")

$manifest = [ordered]@{
  schemaVersion = 2
  artifact = "TakaroConan.pak"
  architecture = "sidecar-datacmd-client-renderer"
  sha256 = $pakHash
  sizeBytes = $pakSize
  builtAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
  buildHost = $env:COMPUTERNAME
  buildReport = "BUILD_REPORT.md"
  devkit = [ordered]@{ distribution = "Conan Exiles DevKit"; branch = $DevKitBranch; version = $DevKitVersion; installPath = $DevKitRoot }
  source = [ordered]@{
    repo = "gettakaro/connectors"; commit = $SourceRevision
    handoffPath = "conan-exiles/mod/TakaroConanBridge/devkit-handoff"
    modName = "TakaroConan"; implementationPlan = "IMPLEMENTATION_PLAN.md"
    sourceContract = "BUILD_SOURCE_CONTRACT.json"; sourceEvidence = "SOURCE_EVIDENCE.md"
  }
  sourceEvidence = [ordered]@{
    report = "SOURCE_EVIDENCE.md"; sourceContract = "BUILD_SOURCE_CONTRACT.json"
    devkitSourceReturned = $true; modControllerDocumented = $true
    dataActorCommandDocumented = $true; commandTableDocumented = $true
    clientRendererDocumented = $true; dataCmdDispatchDocumented = $true
    vanillaInboundChatDocumented = $true; stableInboundIdentityDocumented = $true
    noClientHttpDocumented = $true; noPippiReferencesDocumented = $true
  }
  assets = [ordered]@{
    modController = "Content/Mods/TakaroConan/TakaroConan_ModController"
    dataActorCommand = "Content/Mods/TakaroConan/BP_TakaroChatCommand"
    consoleCommandTable = "Content/Mods/TakaroConan/DT_TakaroConsoleCommands"
  }
  runtimeContract = [ordered]@{
    outboundDispatch = 'con <player-index> dc TakaroChat "<sender>" "<message>"'
    clientRenderer = "W_ChatWindow/W_RichChatLine/FCRichTextBlock"
    clientLogPrefix = "TakaroConan: rendered "; inboundChatSource = "Conan dedicated-server ChatWindow log"
    clientHttpPolling = $false; clientContainsSecrets = $false; inboundChatRequiresStableIdentity = $true
    supportsServerWideChat = $true; supportsTargetedChat = $true; supportsInboundChat = $true
    usesPippiRconChatCommands = $false
  }
  security = [ordered]@{ containsTakaroCloudToken = $false; containsRegistrationToken = $false; containsRconPassword = $false; containsPippiAssets = $false }
  validation = [ordered]@{ compiledInDevKit = $true; cookedInDevKit = $true; validateTakaroPakPassed = $false; installedOnServer = $false; installedOnClient = $false; liveAuditPassed = $false }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -Path (Join-Path $OutputDir "artifact-manifest.json")

$files = @(Get-ChildItem -LiteralPath $OutputDir -File | Select-Object -ExpandProperty Name | Sort-Object)
$expected = @("BUILD_REPORT.md", "SOURCE_EVIDENCE.md", "TakaroConan.pak", "artifact-manifest.json") | Sort-Object
if (Compare-Object $files $expected) { Fail "runtime bundle is not the exact required four files" }

Write-Host "TakaroConan artifact collection ready: $OutputDir"
