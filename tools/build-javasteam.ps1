# PowerShell twin of tools/build-javasteam.sh (spec WP1-1): builds javasteam and
# javasteam-depotdownloader from references/JavaSteam and publishes them into
# build/javasteam-maven as <base>-xrg.<first 8 hex digits of the gitlink>.
# Requires JDK 17 (JAVA_HOME) and network access to Maven Central / the Gradle plugin portal.
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$JS = Join-Path $Root 'references/JavaSteam'
if (-not (Test-Path (Join-Path $JS 'build.gradle.kts'))) {
    throw 'references/JavaSteam is not checked out; run: git submodule update --init references/JavaSteam'
}

$baseMatch = Select-String -Path (Join-Path $JS 'build.gradle.kts') -Pattern '^\s*version = "(.*)-SNAPSHOT"$' | Select-Object -First 1
$rev = (git -C $JS rev-parse HEAD).Substring(0, 8)
if (-not $baseMatch -or -not $rev) { throw 'cannot derive the JavaSteam version' }
$version = "$($baseMatch.Matches[0].Groups[1].Value)-xrg.$rev"

$catalogMatch = Select-String -Path (Join-Path $Root 'gradle/libs.versions.toml') -Pattern '^javasteam = "([^"]*)"' | Select-Object -First 1
$catalog = $catalogMatch.Matches[0].Groups[1].Value
if ($version -ne $catalog) {
    throw "gradle/libs.versions.toml has javasteam = `"$catalog`", but references/JavaSteam builds $version. Update the catalog (or the gitlink) so they match."
}

$repo = Join-Path $Root 'build/javasteam-maven'
New-Item -ItemType Directory -Force $repo | Out-Null
$init = Join-Path $Root 'tools/javasteam/publish-local.init.gradle'

Write-Host "==> JavaSteam $version from $(git -C $JS rev-parse HEAD)"
Push-Location $JS
try {
    & .\gradlew.bat --no-daemon --console=plain `
        --init-script $init `
        "-Dxrgame.javasteam.version=$version" `
        "-Dxrgame.javasteam.repo=$repo" `
        :publishMavenJavaPublicationToXrgameRepository `
        :javasteam-depotdownloader:publishMavenJavaPublicationToXrgameRepository
    if ($LASTEXITCODE -ne 0) { throw "JavaSteam build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}

Write-Host '==> Published to build/javasteam-maven:'
Get-ChildItem $repo -Recurse -File | Where-Object { $_.Name -like "*-$version.jar" -or $_.Name -like "*-$version.pom" } |
    Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.FullName.Substring($Root.Length + 1)
    }
