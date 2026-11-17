<#
.SYNOPSIS
    Build the Geode package for Geometry Dash 2.209 (Windows) from nothing but this checkout and the network: the CI job's steps, so that a desk run proves them.

.DESCRIPTION
    Geode's own action cannot build this: the mod is made for a 2.209 executable, which needs (1) the bindings of a fork of geode-sdk/bindings that
    carries bindings/2.209, and (2) the Geode SDK with the header changes in tools/geode-sdk-2209.patch (2.209's cocos2d layout differs from 2.2081's
    in a few places the SDK's inline code and class layouts depend on). The steps, in order:

      1. clone the SDK at a pinned commit (v5.10.1) and `git apply` tools/geode-sdk-2209.patch
      2. install the SDK's prebuilt loader binaries with the Geode CLI
      3. clone the bindings fork at a ref
      4. configure and build with the options below, and collect the .geode

    Nothing outside -Work is touched: the SDK is a clone in -Work (GEODE_SDK is set for this process only; `geode sdk install` / `set-path`, which would
    change the CLI's own configuration, are not used).

.PARAMETER Work           Directory for the clones and the build (default ./ci-work).
.PARAMETER BindingsRepo   URL or path of the fork of geode-sdk/bindings that carries bindings/2.209 (env GDSOLVER_BINDINGS_REPO).
.PARAMETER BindingsRef    Branch, tag or commit of the fork to build against (env GDSOLVER_BINDINGS_REF).
.PARAMETER Comp           GEODE_COMP_GD_VERSION: 22090 = the first 2.209 executable, 22091 = the hotfix executable (default).
.PARAMETER Compiler       clang (what the CI job uses: Ninja + clang/clang++, Release, LTO, as geode-sdk/build-geode-mod does on Windows) or msvc
                          (Visual Studio generator; for a desk without clang on PATH).
.PARAMETER Out            Where the .geode and provenance.json are collected (default <Work>/output).
.PARAMETER CodegenCache   A directory that keeps the Codegen between runs (the CI job restores and saves it with actions/cache). The bindings' CMake otherwise
                          downloads the prebuilt Codegen of a rolling release of geode-sdk/bindings, and the one published now writes `#include <Geode/mapping/...>`
                          headers that the bindings and SDK pinned here do not have: the build stops at the first of them. So the Codegen is built from the
                          bindings' own source (-DSKIP_BUILDING_CODEGEN=OFF; about half a minute with clang and Ninja on a desk, about three with the Visual Studio
                          generator), which is the Codegen of that bindings commit by construction (the headers it writes were compared file by file with the earlier
                          prebuilt's, 3057 files: no difference). With a cache, the built Codegen.exe is saved here with a
                          codegen.id naming the bindings commit and the compiler; a later run whose id is the same puts it into the build and passes
                          -DSKIP_BUILDING_CODEGEN=ON, which makes CMake use it as it is (it asks neither for its age nor for a download). Without one it is built every run.
                          Not yet exercised on GitHub: that needs a push, and the first release run after this change is the check (its log says whether the
                          Codegen was restored from the cache or built from the bindings source).

.EXAMPLE
    ./scripts/ci-build-2209.ps1 -BindingsRepo https://github.com/<owner>/bindings -BindingsRef <ref>
#>
param(
    [string]$Work = (Join-Path (Get-Location).Path 'ci-work'),
    [string]$BindingsRepo = $env:GDSOLVER_BINDINGS_REPO,
    [string]$BindingsRef = $env:GDSOLVER_BINDINGS_REF,
    [string]$SdkRepo = 'https://github.com/geode-sdk/geode',
    [string]$SdkCommit = '7e41336f68660b990644f5c3450b6da6ae944560',   # v5.10.1; the patch is made against exactly this tree
    [string]$GdVersion = '2.209',
    [int]$Comp = 22091,
    [ValidateSet('clang', 'msvc')][string]$Compiler = 'clang',
    [string]$Out = '',
    [string]$CodegenCache = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $Work 'output' }
if (-not $BindingsRepo -or -not $BindingsRef) { throw 'give -BindingsRepo and -BindingsRef (the fork of geode-sdk/bindings with bindings/2.209, and the ref to build against)' }

function Step([string]$m) { Write-Host "`n== $m" -ForegroundColor Cyan }
function Run([string]$exe, [string[]]$a) {
    & $exe @a
    if ($LASTEXITCODE -ne 0) { throw "$exe $($a -join ' ') exited with $LASTEXITCODE" }
}
foreach ($t in 'git', 'cmake', 'geode') { if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t is not on PATH" } }
if ($Compiler -eq 'clang') {
    foreach ($t in 'clang', 'clang++', 'ninja') { if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t is not on PATH (use -Compiler msvc, or install LLVM and Ninja)" } }
}
$modJson = Get-Content (Join-Path $repo 'mod.json') -Raw | ConvertFrom-Json
$geodeVersion = $modJson.geode
New-Item -ItemType Directory -Force $Work | Out-Null
$sdk = Join-Path $Work 'geode-sdk'
$bnd = Join-Path $Work 'bindings'
$build = Join-Path $Work 'build'

Step "Geode SDK $geodeVersion at $($SdkCommit.Substring(0, 8)) + tools/geode-sdk-2209.patch"
$patch = Join-Path $repo 'tools\geode-sdk-2209.patch'
if (-not (Test-Path (Join-Path $sdk '.git'))) {
    # autocrlf off: the patch is made against the repository's own bytes, and a Windows checkout would rewrite the line endings under it
    Run git @('clone', '--quiet', '--no-checkout', '--config', 'core.autocrlf=false', $SdkRepo, $sdk)
}
Run git @('-C', $sdk, 'checkout', '--quiet', '--detach', $SdkCommit)
if ((& git -C $sdk rev-parse HEAD) -ne $SdkCommit) { throw 'the SDK is not at the pinned commit' }
$ErrorActionPreference = 'Continue'      # git writes "patch failed" to stderr when the answer is simply no
& git -C $sdk apply --check -R $patch 2>$null     # already applied (a re-run)?
$applied = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = 'Stop'
if ($applied) {
    Write-Host 'the patch is already applied'
} else {
    Run git @('-C', $sdk, 'apply', '--check', $patch)
    Run git @('-C', $sdk, 'apply', $patch)
}
Run git @('-C', $sdk, 'diff', '--stat')
$env:GEODE_SDK = $sdk

Step "SDK binaries ($geodeVersion, win)"
if (-not (Test-Path (Join-Path $sdk "bin\$geodeVersion"))) {
    Run geode @('sdk', 'install-binaries', '--platform', 'win', '--version', $geodeVersion)
}
if (-not (Test-Path (Join-Path $sdk "bin\$geodeVersion"))) { throw "no SDK binaries for $geodeVersion in $sdk\bin" }

Step "bindings: $BindingsRepo @ $BindingsRef"
if (-not (Test-Path (Join-Path $bnd '.git'))) { Run git @('clone', '--quiet', $BindingsRepo, $bnd) }
else { Run git @('-C', $bnd, 'fetch', '--quiet', '--all') }
$bsha = (& git -C $bnd rev-parse --verify --quiet "origin/$BindingsRef^{commit}" 2>$null)       # a branch of the fork
if (-not $bsha) { $bsha = (& git -C $bnd rev-parse --verify --quiet "$BindingsRef^{commit}" 2>$null) }   # a tag or a commit
if (-not $bsha) { throw "the fork has no ref $BindingsRef" }
Run git @('-C', $bnd, 'checkout', '--quiet', '--detach', $bsha)
if (-not (Test-Path (Join-Path $bnd "bindings\$GdVersion"))) { throw "bindings/$GdVersion is not in the fork at that ref" }
$env:GEODE_BINDINGS_REPO_PATH = $bnd

Step "configure and build ($Compiler, GD $GdVersion, GEODE_COMP_GD_VERSION $Comp)"
if (Test-Path $build) { Remove-Item $build -Recurse -Force }
$cfg = @('-S', $repo, '-B', $build, "-DGEODE_DONT_INSTALL_MODS=ON", "-DGEODE_GD_VERSION=$GdVersion", "-DGEODE_COMP_GD_VERSION=$Comp")
if ($Compiler -eq 'clang') {
    # what geode-sdk/build-geode-mod passes on Windows, and two additions:
    #  -DCMAKE_LINKER_TYPE=LLD  CMake >= 4 otherwise passes -fuse-ld=lld-link, which a recent clang rejects as an invalid linker name
    #  -DGDSOLVER_BUILD_DP=OFF  the package is the mod; the solver's command-line tool and its test drivers are not part of it (the mod's own copy
    #                           of the solver core is built either way)
    $cfg += @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
              '-DGEODE_CODEGEN_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang;-DCMAKE_CXX_COMPILER=clang++;-G Ninja',
              '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON', '-DGEODE_TARGET_PLATFORM=Win64',
              '-DCMAKE_LINKER_TYPE=LLD', '-DGDSOLVER_BUILD_DP=OFF')
    $buildArgs = @('--build', $build, '--config', 'Release', '--parallel')
} else {
    $cfg += @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    $buildArgs = @('--build', $build, '--config', 'RelWithDebInfo', '--parallel')
}
# The Codegen. Never the prebuilt that the bindings' CMake downloads (see -CodegenCache): built from the bindings' own source, or restored from the cache.
$cgExe = Join-Path $build 'bindings\codegen\Codegen.exe'
$cgId = "bindings=$bsha compiler=$Compiler"
$cacheExe = if ($CodegenCache) { Join-Path $CodegenCache 'Codegen.exe' } else { '' }
$cacheId = if ($CodegenCache) { Join-Path $CodegenCache 'codegen.id' } else { '' }
$fromCache = $false
if ($cacheExe -and (Test-Path $cacheExe)) {
    $have = if (Test-Path $cacheId) { (Get-Content $cacheId -Raw).Trim() } else { '(no codegen.id)' }
    if ($have -eq $cgId) {
        New-Item -ItemType Directory -Force (Split-Path $cgExe) | Out-Null
        Copy-Item $cacheExe $cgExe -Force
        $fromCache = $true
        Write-Host "Codegen restored from the cache ($cgId, sha256 $((Get-FileHash $cgExe -Algorithm SHA256).Hash.Substring(0, 12)))"
    } else {
        Write-Host "the Codegen cache is for '$have', this run is '$cgId': building the Codegen from the bindings source"
    }
}
$cfg += ('-DSKIP_BUILDING_CODEGEN=' + $(if ($fromCache) { 'ON' } else { 'OFF' }))
Run cmake $cfg
if (-not (Test-Path $cgExe)) { throw "no Codegen at $cgExe after configure" }
if ($cacheExe -and -not $fromCache) {
    New-Item -ItemType Directory -Force $CodegenCache | Out-Null
    Copy-Item $cgExe $cacheExe -Force
    Set-Content $cacheId $cgId -Encoding ascii
    Write-Host "Codegen built from the bindings source and saved to $CodegenCache"
}
Run cmake $buildArgs

Step "collect"
New-Item -ItemType Directory -Force $Out | Out-Null
$pkgs = @(Get-ChildItem $build -Recurse -Filter '*.geode')
if ($pkgs.Count -eq 0) { throw 'no .geode was built' }
foreach ($p in $pkgs) { Copy-Item $p.FullName $Out -Force }
$prov = [ordered]@{
    commit        = (& git -C $repo rev-parse HEAD)
    dirty         = [bool](& git -C $repo status --porcelain)
    gd            = $GdVersion; comp = $Comp; compiler = $Compiler
    sdk_commit    = $SdkCommit
    sdk_patch_sha256 = (Get-FileHash $patch -Algorithm SHA256).Hash
    bindings      = $BindingsRepo; bindings_ref = $BindingsRef; bindings_commit = (& git -C $bnd rev-parse HEAD)
    codegen       = $(if ($fromCache) { 'restored from the cache' } else { 'built from the bindings source' }); codegen_sha256 = (Get-FileHash $cgExe -Algorithm SHA256).Hash
    packages      = @($pkgs | ForEach-Object { [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash (Join-Path $Out $_.Name) -Algorithm SHA256).Hash } })
}
$prov | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'provenance.json') -Encoding ascii
$prov | ConvertTo-Json -Depth 4
Write-Host "`npackage(s) in $Out"
