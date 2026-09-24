#!/usr/bin/env bash
# Builds javasteam and javasteam-depotdownloader from references/JavaSteam (spec WP1-1) and
# publishes them into build/javasteam-maven with a fixed version:
#   <JavaSteam base version>-xrg.<first 8 hex digits of the references/JavaSteam gitlink>
# settings.gradle.kts resolves io.github.joshuatam only from that directory, and
# gradle/libs.versions.toml must name the same version. Run this before building the app.
#
# Requires JDK 17 (JAVA_HOME) and network access to Maven Central / the Gradle plugin portal.
# Works in Git Bash on Windows and on Linux. tools/build-javasteam.ps1 is the PowerShell twin.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JS="$ROOT/references/JavaSteam"
[ -f "$JS/build.gradle.kts" ] || {
    echo "references/JavaSteam is not checked out; run: git submodule update --init references/JavaSteam" >&2
    exit 1
}

# Java needs native paths; `pwd -W` gives C:/... under Git Bash and is absent on Linux.
native() { (cd "$1" && pwd -W 2>/dev/null) || (cd "$1" && pwd); }

base="$(sed -n 's/^ *version = "\(.*\)-SNAPSHOT"$/\1/p' "$JS/build.gradle.kts")"
rev="$(git -C "$JS" rev-parse HEAD | cut -c1-8)"
[ -n "$base" ] && [ -n "$rev" ] || { echo "cannot derive the JavaSteam version" >&2; exit 1; }
version="$base-xrg.$rev"

catalog="$(sed -n 's/^javasteam = "\([^"]*\)".*/\1/p' "$ROOT/gradle/libs.versions.toml")"
if [ "$version" != "$catalog" ]; then
    echo "gradle/libs.versions.toml has javasteam = \"$catalog\", but references/JavaSteam builds $version." >&2
    echo "Update the catalog (or the gitlink) so they match." >&2
    exit 1
fi

mkdir -p "$ROOT/build/javasteam-maven"
repo="$(native "$ROOT/build/javasteam-maven")"
init="$(native "$ROOT/tools/javasteam")/publish-local.init.gradle"

echo "==> JavaSteam $version from $(git -C "$JS" rev-parse HEAD)"
cd "$JS"
./gradlew --no-daemon --console=plain \
    --init-script "$init" \
    "-Dxrgame.javasteam.version=$version" \
    "-Dxrgame.javasteam.repo=$repo" \
    :publishMavenJavaPublicationToXrgameRepository \
    :javasteam-depotdownloader:publishMavenJavaPublicationToXrgameRepository

echo "==> Published to build/javasteam-maven:"
find "$ROOT/build/javasteam-maven" -name "*-$version.jar" -o -name "*-$version.pom" | sort | while read -r f; do
    sha256sum "$f" | sed "s|$ROOT/||"
done
