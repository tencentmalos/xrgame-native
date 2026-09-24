pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // JavaSteam is built from references/JavaSteam by tools/build-javasteam.sh (spec WP1-1),
        // never fetched from the Sonatype snapshots repository.
        exclusiveContent {
            forRepository {
                maven {
                    name = "javasteamLocal"
                    url = uri("build/javasteam-maven")
                }
            }
            filter { includeGroup("io.github.joshuatam") }
        }
    }
}

if (!file("build/javasteam-maven/io/github/joshuatam").isDirectory) {
    logger.warn("build/javasteam-maven is missing: run tools/build-javasteam.sh (or .ps1) before building the app")
}

rootProject.name = "gamenative"
include(":app")
include(":ubuntufs")
