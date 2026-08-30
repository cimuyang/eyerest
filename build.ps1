param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    $knownPaths = @(
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\CMake\bin\cmake.exe"
    )
    $cmakePath = $knownPaths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $cmakePath) {
        throw '没有找到 CMake。请安装 Visual Studio 2022，并勾选“使用 C++ 的桌面开发”和 CMake 工具。'
    }
} else {
    $cmakePath = $cmake.Source
}

& $cmakePath -S $projectRoot -B $buildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }

& $cmakePath --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw '编译失败。' }

$executable = Join-Path $buildDirectory "$Configuration\EyeRest.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    $executable = Get-ChildItem -Path $buildDirectory -Filter EyeRest.exe -Recurse | Select-Object -First 1 -ExpandProperty FullName
}

Write-Host "构建完成：$executable" -ForegroundColor Green
