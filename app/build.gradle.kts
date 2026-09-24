import java.util.Properties
import java.io.FileInputStream

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.dagger.hilt)
    alias(libs.plugins.jetbrains.kotlin.android)
    alias(libs.plugins.jetbrains.serialization)
    alias(libs.plugins.kotlinter)
    alias(libs.plugins.ksp)
    alias(libs.plugins.secrets.gradle)
    alias(libs.plugins.room)
}

val keystorePropertiesFile = rootProject.file("app/keystores/keystore.properties")
val keystoreProperties: Properties? = if (keystorePropertiesFile.exists()) {
    Properties().apply {
        load(FileInputStream(keystorePropertiesFile))
    }
} else null

// XRGame Native (picoXr) release signing. Read only from this gitignored file, never from the
// repo-root `keystore` or the upstream "pluvia" config. Without it, picoXr release builds fail
// at signing instead of falling back to another key; debug builds use the local debug keystore.
val xrgameKeystorePropertiesFile = rootProject.file("app/keystores/xrgame.properties")
val xrgameKeystoreProperties: Properties? = if (xrgameKeystorePropertiesFile.exists()) {
    Properties().apply {
        load(FileInputStream(xrgameKeystorePropertiesFile))
    }
} else null

// Add PostHog API key and host as build-time variables
val posthogApiKey: String = project.findProperty("POSTHOG_API_KEY") as String? ?: System.getenv("POSTHOG_API_KEY") ?: ""
val posthogHost: String = project.findProperty("POSTHOG_HOST") as String? ?: System.getenv("POSTHOG_HOST") ?: "https://us.i.posthog.com"

val metaAppId: String = project.findProperty("META_APP_ID") as String? ?: System.getenv("META_APP_ID") ?: ""
val productSku: String = project.findProperty("PRODUCT_SKU") as String? ?: System.getenv("PRODUCT_SKU") ?: ""

room {
    schemaDirectory("$projectDir/schemas")
}

// Debug-only: package the repo's manifest.json so debug builds read it locally (never in release).
val copyDebugManifest by tasks.registering(Copy::class) {
    from(rootProject.file("manifest.json"))
    into(layout.buildDirectory.dir("generated/debugManifest"))
}

android {
    namespace = "app.gamenative"
    compileSdk = 36

    // https://developer.android.com/ndk/downloads
    ndkVersion = "27.3.13750724"

    signingConfigs {
        create("pluvia") {
            if (keystoreProperties != null) {
                storeFile = file(keystoreProperties["storeFile"].toString())
                storePassword = keystoreProperties["storePassword"].toString()
                keyAlias = keystoreProperties["keyAlias"].toString()
                keyPassword = keystoreProperties["keyPassword"].toString()
            }
        }
        create("xrgame") {
            if (xrgameKeystoreProperties != null) {
                storeFile = file(xrgameKeystoreProperties["storeFile"].toString())
                storePassword = xrgameKeystoreProperties["storePassword"].toString()
                keyAlias = xrgameKeystoreProperties["keyAlias"].toString()
                keyPassword = xrgameKeystoreProperties["keyPassword"].toString()
            }
        }
    }

    defaultConfig {
        applicationId = "app.gamenative"

        minSdk = 26

        manifestPlaceholders["screenOrientation"] = "unspecified"
        buildConfigField("boolean", "XR_BUILD", "false")
        buildConfigField("boolean", "MODERN_XR", "false")
        // XRGame Native (picoXr) behavior switch; see app.gamenative.xrgame.
        buildConfigField("boolean", "XRGAME", "false")

        versionCode = 23
        versionName = "1.2.1"

        buildConfigField("boolean", "GOLD", "false")
        fun secret(name: String) =
            project.findProperty(name) as String? ?: System.getenv(name) ?: ""

        buildConfigField("String", "POSTHOG_API_KEY", "\"${secret("POSTHOG_API_KEY")}\"")
        buildConfigField("String", "POSTHOG_HOST",  "\"${secret("POSTHOG_HOST")}\"")
        buildConfigField("String", "STEAMGRIDDB_API_KEY", "\"${secret("STEAMGRIDDB_API_KEY")}\"")
        buildConfigField("String", "CLOUD_PROJECT_NUMBER", "\"${secret("CLOUD_PROJECT_NUMBER")}\"")
        val iconValue = "@mipmap/ic_launcher"
        val iconRoundValue = "@mipmap/ic_launcher_round"
        manifestPlaceholders.putAll(
            mapOf(
                "icon" to iconValue,
                "roundIcon" to iconRoundValue,
            ),
        )

        ndk {
            //abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a"))
        }

        // Localization support - specify which languages to include
        resourceConfigurations += listOf(
            "en",      // English (default)
            "es",      // Spanish
            "da",      // Danish
            "pt-rBR",  // Portuguese (Brazilian)
            "zh-rTW",  // Traditional Chinese
            "zh-rCN",  // Simplified Chinese
            "fr",      // French
            "de",      // German
            "uk",      // Ukrainian
            "it",      // Italian
            "ro",      // Română
            "pl",      // Polish
            "ru",      // Russian
            "ko",      // Korean
            "ja",      // Japanese
            // TODO: Add more languages here using the ISO 639-1 locale code with regional qualifiers (e.g., "pt-rPT" for European Portuguese)
        )

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }

        proguardFiles(
            // getDefaultProguardFile("proguard-android-optimize.txt"),
            getDefaultProguardFile("proguard-android.txt"),
            "proguard-rules.pro",
        )
    }

    flavorDimensions += "androidApi"
    productFlavors {
        create("legacy") {
            dimension = "androidApi"
            targetSdk = 28
            ndk.abiFilters += listOf("arm64-v8a", "armeabi-v7a")
            buildConfigField("boolean", "MODERN_ANDROID", "false")
            buildConfigField("String", "PRELOAD_BIONIC_SO", "\"libredirect-bionic.so\"")
        }
        create("legacyXr") {
            dimension = "androidApi"
            targetSdk = 28
            ndk.abiFilters += listOf("arm64-v8a", "armeabi-v7a")
            buildConfigField("boolean", "MODERN_ANDROID", "false")
            buildConfigField("String", "PRELOAD_BIONIC_SO", "\"libredirect-bionic.so\"")
            buildConfigField("boolean", "XR_BUILD", "true")
            manifestPlaceholders["screenOrientation"] = "landscape"
        }
        create("modern") {
            dimension = "androidApi"
            minSdk = 29
            targetSdk = 36
            ndk.abiFilters += listOf("arm64-v8a")
            buildConfigField("boolean", "MODERN_ANDROID", "true")
            buildConfigField("String", "PRELOAD_BIONIC_SO", "\"libredirect-bionic-wx.so\"")
        }
        create("modernXr") {
            dimension = "androidApi"
            minSdk = 29
            targetSdk = 36
            ndk.abiFilters += listOf("arm64-v8a")
            buildConfigField("boolean", "MODERN_ANDROID", "true")
            buildConfigField("String", "PRELOAD_BIONIC_SO", "\"libredirect-bionic-wx.so\"")
            buildConfigField("boolean", "XR_BUILD", "true")
            buildConfigField("boolean", "MODERN_XR", "true")
            buildConfigField("String", "META_APP_ID", "\"$metaAppId\"")
            buildConfigField("String", "PRODUCT_SKU", "\"$productSku\"")
            manifestPlaceholders["screenOrientation"] = "landscape"
        }
        // XRGame Native for Pico headsets (docs/specs/xrgame-native-v1.md). Starts as a copy of
        // `modern` with its own identity; XR adaptation comes in spec WP5.
        create("picoXr") {
            dimension = "androidApi"
            applicationId = "com.tencentmalos.xrgamenative"
            minSdk = 29
            targetSdk = 36
            ndk.abiFilters += listOf("arm64-v8a")
            buildConfigField("boolean", "MODERN_ANDROID", "true")
            buildConfigField("String", "PRELOAD_BIONIC_SO", "\"libredirect-bionic-wx.so\"")
            buildConfigField("boolean", "XRGAME", "true")
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
            isMinifyEnabled = false
            isShrinkResources = false
            signingConfig = signingConfigs.getByName("debug")
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("debug")
        }
        create("release-signed") {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("pluvia")
        }
        create("release-gold") {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("pluvia")
            applicationIdSuffix = ".gold"
            buildConfigField("boolean", "GOLD", "true")
            val iconValue = "@mipmap/ic_launcher_gold"
            val iconRoundValue = "@mipmap/ic_launcher_gold_round"
            manifestPlaceholders.putAll(
                mapOf(
                    "icon" to iconValue,
                    "roundIcon" to iconRoundValue,
                ),
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
        // Exposes the openxr_loader_for_android AAR's native headers/lib to CMake, for the
        // (not yet wired into the default build — see xrimmersive/CMakeLists.txt) immersive
        // VR native module.
        prefab = true
    }

    packaging {
        resources {
            excludes += "/DebugProbesKt.bin"
            excludes += "/junit/runner/smalllogo.gif"
            excludes += "/junit/runner/logo.gif"
            excludes += "/META-INF/versions/9/OSGI-INF/MANIFEST.MF"
        }
        jniLibs {
            // 'extractNativeLibs' was not enough to keep the jniLibs and
            // the libs went missing after adding on-demand feature delivery
            useLegacyPackaging = true
        }
    }
    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            all {
                it.maxHeapSize = "4g"
                it.testLogging { events("started", "failed") }
            }
        }
    }

    lint {
        // Locale files ship full AndroidX appcompat (abc_*) translations that aren't in the
        // default locale. These extra translations are harmless and pre-existing; without this
        // the release-only lintVital pass fails on 150+ ExtraTranslation errors.
        disable += "ExtraTranslation"
    }
    dynamicFeatures += setOf(":ubuntufs")

    // Configure Assets to be used in different variants
    sourceSets {
        getByName("legacy") {
            java.srcDir("src/nonXr/java")
            assets {
                srcDirs("src/legacy/assets", "src/main/assets")
            }
        }
        getByName("legacyXr") {
            java.srcDir("src/nonXr/java")
            // Superset of src/legacy/AndroidManifest.xml plus the immersive VR entries —
            // keep the shared parts in sync with that file.
            manifest.srcFile("src/legacyXr/AndroidManifest.xml")
            assets {
                srcDirs("src/legacy/assets", "src/main/assets")
            }
            jniLibs {
                srcDirs("src/legacy/jniLibs", "src/legacyXr/jniLibs")
            }
        }
        getByName("modern") {
            java.srcDir("src/nonXr/java")
            assets {
                srcDirs("src/modern/assets", "src/main/assets")
            }
        }
        getByName("modernXr") {
            assets {
                srcDirs("src/modern/assets", "src/main/assets")
            }
            jniLibs {
                setSrcDirs(listOf("src/modern/jniLibs", "src/modernXr/jniLibs"))
            }
        }
        // Same inputs as `modern`; src/picoXr only adds the manifest and identity resources.
        getByName("picoXr") {
            java.srcDir("src/nonXr/java")
            assets {
                srcDirs("src/modern/assets", "src/main/assets")
            }
            jniLibs {
                srcDirs("src/modern/jniLibs")
            }
        }
        getByName("debug") {
            assets.srcDir(copyDebugManifest)
        }
    }

    kotlinter {
        ignoreFormatFailures  = false
    }

    val hostCanRunXrPayloadScripts = System.getProperty("os.name").startsWith("Windows")

    tasks.register<Exec>("buildModernXrNative") {
        enabled = hostCanRunXrPayloadScripts
        commandLine(
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            rootProject.file("tools/build-xr-native.ps1").absolutePath,
        )
    }

    tasks.register<Exec>("buildWindowsXrRuntime") {
        enabled = hostCanRunXrPayloadScripts
        commandLine(
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            rootProject.file("tools/build-windows-xr-runtime.ps1").absolutePath,
        )
    }

    tasks.register<Exec>("stageWineXrBridge") {
        enabled = hostCanRunXrPayloadScripts
        dependsOn("buildModernXrNative")
        val companion = providers.environmentVariable("GAMENATIVE_WINE_XR_BRIDGE")
        doFirst {
            check(companion.isPresent) { "GAMENATIVE_WINE_XR_BRIDGE must point to the ARM64X Wine builtin companion" }
        }
        commandLine(
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            rootProject.file("tools/stage-wine-xr-bridge.ps1").absolutePath,
            "-CompanionPath",
            companion.getOrElse(""),
        )
    }

    tasks.register<Exec>("stageOpenComposite") {
        enabled = hostCanRunXrPayloadScripts
        commandLine(
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            rootProject.file("tools/stage-opencomposite.ps1").absolutePath,
        )
    }

    tasks.register<Exec>("verifyModernXrPayload") {
        enabled = hostCanRunXrPayloadScripts
        dependsOn("buildModernXrNative", "buildWindowsXrRuntime", "stageWineXrBridge", "stageOpenComposite")
        commandLine(
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            rootProject.file("tools/verify-xr-payload.ps1").absolutePath,
        )
    }

    tasks.register("prepareModernXrPayload") {
        dependsOn("verifyModernXrPayload")
    }


    // externalNativeBuild {
    //   cmake {
    //       path = file("src/main/cpp/asurfacerenderer/CMakeLists.txt")
    //   }
    // }

    // externalNativeBuild {
    //    cmake {
    //        path = file("src/main/cpp/evshim/CMakeLists.txt")
    //    }
    // }

    // xconnectorpatch is shipped as a prebuilt jniLib because our APK packaging flow
    // does not rebuild native libraries during release creation.
    // externalNativeBuild {
    //     cmake {
    //         path = file("src/main/cpp/xconnectorpatch/CMakeLists.txt")
    //         version = "3.22.1"
    //     }
    // }

    // build extras needed in libwinlator_bionic.so
    // externalNativeBuild {
    //     cmake {
    //         path = file("src/main/cpp/extras/CMakeLists.txt")   // the file shown above
    //         version = "3.22.1"
    //     }
    // }

    // cmake on release builds a proot that fails to process ld-2.31.so
    // externalNativeBuild {
    //     cmake {
    //         path = file("src/main/cpp/CMakeLists.txt")
    //         version = "3.22.1"
    //     }
    // }

    // Meta Quest immersive launch mode's native OpenXR module. Same convention as the
    // other native modules above: not part of the default build (native libs ship as
    // prebuilt .so files in jniLibs/) — temporarily uncomment to build+test locally,
    // then copy the resulting libxrimmersive.so into jniLibs/arm64-v8a/ and re-comment.
    // externalNativeBuild {
    //     cmake {
    //         path = file("src/main/cpp/xrimmersive/CMakeLists.txt")
    //         version = "3.22.1"
    //     }
    // }

    // (For now) Uncomment for LeakCanary to work.
    // configurations {
    //     debugImplementation {
    //         exclude(group = "junit", module = "junit")
    //     }
    // }
}

dependencies {
    implementation(libs.material)

    // Chrome Custom Tabs for GOG OAuth
    implementation("androidx.browser:browser:1.8.0")
    implementation("androidx.documentfile:documentfile:1.0.1")

    // JavaSteam
    val localBuild = false // Change to 'true' needed when building JavaSteam manually
    if (localBuild) {
        implementation(files("../../JavaSteam/build/libs/javasteam-1.8.0.1-26-SNAPSHOT.jar"))
        implementation(files("../../JavaSteam/javasteam-depotdownloader/build/libs/javasteam-depotdownloader-1.8.0.1-26-SNAPSHOT.jar"))
        implementation(libs.bundles.javasteam.dev)
    } else {
        implementation(libs.javasteam) {
            isChanging = version?.contains("SNAPSHOT") ?: false
        }
        implementation(libs.javasteam.depotdownloader) {
            isChanging = version?.contains("SNAPSHOT") ?: false
        }
    }
    implementation(libs.spongycastle)
    implementation(libs.okhttp.dnsoverhttps)

    // Split Modules
    implementation(libs.bundles.google)

    // Official Khronos OpenXR loader (Apache-2.0) for the Meta Quest immersive launch mode's
    // native module (app/src/main/cpp/xrimmersive) — not a Winlator/GameNativeXR dependency.
    "modernXrImplementation"("org.khronos.openxr:openxr_loader_for_android:1.1.61")

    // Winlator
    implementation(libs.bundles.winlator)
    implementation(libs.libarchive.android)
    implementation(libs.zstd.jni) { artifact { type = "aar" } }
    implementation(libs.xz)

    // Jetpack Compose
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.bundles.compose)
    implementation(libs.landscapist.coil)
    implementation(libs.media3.exoplayer)
    implementation(libs.media3.exoplayer.hls)
    implementation(libs.media3.ui)
    debugImplementation(libs.androidx.ui.tooling)

    // Support
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.apng)
    implementation(libs.datastore.preferences)
    implementation(libs.jetbrains.kotlinx.json)
    implementation(libs.kotlin.coroutines)
    implementation(libs.timber)
    implementation(libs.zxing)

    // Google Protobufs
    implementation(libs.protobuf.java)

    // Hilt
    implementation(libs.bundles.hilt)

    // KSP (Hilt, Room)
    ksp(libs.bundles.ksp)

    // Room Database
    implementation(libs.bundles.room)

    // Memory Leak Detection
    // debugImplementation("com.squareup.leakcanary:leakcanary-android:3.0-alpha-8")

    // Testing
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.room.testing)
    androidTestImplementation(libs.androidx.runner)
    androidTestImplementation(libs.androidx.ui.test.junit4)
    debugImplementation(libs.androidx.ui.test.manifest)
    testImplementation(libs.junit)
    testImplementation(libs.robolectric)
    testImplementation(libs.mockito.core)
    testImplementation(libs.mockito.kotlin)
    testImplementation(libs.mockk)
    testImplementation(libs.androidx.ui.test.junit4)
    testImplementation(libs.zstd.jni)
    testImplementation(libs.orgJson)
    testImplementation(libs.mockwebserver)

    // Add PostHog Android SDK dependency
    implementation("com.posthog:posthog-android:3.8.0")

    implementation("com.auth0.android:jwtdecode:2.0.2")

    // Samsung Performance SDK
    implementation(files("src/main/lib/perfsdk-v1.0.0.jar"))

    "modernXrImplementation"("com.meta.horizon.platform.sdk:core-kotlin:0.2.2")
    "modernXrImplementation"("com.meta.horizon.platform.sdk:iap-kotlin:0.2.2")
}

// ---- XRGame Native (picoXr) --------------------------------------------------------------
// Kept together at the end of the file so upstream merges rarely touch it.

androidComponents {
    // picoXr ships as debug + release only: release-signed uses the upstream "pluvia" key and
    // release-gold is the upstream store build with its own icon and application id suffix.
    beforeVariants(selector().withFlavor("androidApi" to "picoXr")) { variant ->
        if (variant.buildType == "release-signed" || variant.buildType == "release-gold") {
            variant.enable = false
        }
    }
    // ManifestIdCorrelationTest downloads every entry of the upstream component manifest from
    // upstream servers. picoXr must not contact them (spec C3), and XrGameEgress blocks the
    // download inside Robolectric too, so the test cannot pass for this flavor.
    onVariants(selector().withFlavor("androidApi" to "picoXr")) { variant ->
        tasks.withType<Test>().matching { it.name == "test${variant.name.replaceFirstChar { c -> c.uppercase() }}UnitTest" }
            .configureEach { filter.excludeTestsMatching("app.gamenative.utils.ManifestIdCorrelationTest") }
    }
    // The release build type signs with the debug key for every flavor; picoXr signs with its
    // own key instead (see xrgameKeystorePropertiesFile above).
    onVariants(selector().withFlavor("androidApi" to "picoXr").withBuildType("release")) { variant ->
        variant.signingConfig.setConfig(android.signingConfigs.getByName("xrgame"))
    }
}

/**
 * Builds libgndownload.so from the in-repo Rust crate with cargo-ndk (spec WP1-2) instead of
 * packaging the prebuilt copy in src/main/jniLibs. The crate's .cargo/config.toml keeps its
 * soname and 16 KB max-page-size flags; `--config` appends a GNU build-id so validation records
 * can identify the exact binary (spec C5).
 */
abstract class CargoNdkBuildTask : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val crateSources: ConfigurableFileCollection

    @get:Internal
    abstract val crateDir: DirectoryProperty

    @get:Input
    abstract val ndkDir: Property<String>

    @get:Input
    abstract val cargo: Property<String>

    @get:Internal
    abstract val cargoTargetDir: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:OutputFile
    abstract val buildInfo: RegularFileProperty

    @get:javax.inject.Inject
    abstract val execOps: ExecOperations

    @TaskAction
    fun build() {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        val cargoBin = cargo.get()
        // Toolchain identity for the validation record; exec closes its output stream, so each
        // command writes its own temporary file.
        val info = buildInfo.get().asFile
        info.parentFile.mkdirs()
        info.writeText("")
        for (args in listOf(listOf("--version"), listOf("ndk", "--version"))) {
            val tmp = File(temporaryDir, "version.txt")
            tmp.outputStream().use { os ->
                execOps.exec {
                    commandLine(listOf(cargoBin) + args)
                    standardOutput = os
                }
            }
            info.appendText(tmp.readText())
        }
        info.appendText("ndk: " + ndkDir.get() + "\n")
        execOps.exec {
            workingDir = crateDir.get().asFile
            environment("ANDROID_NDK_HOME", ndkDir.get())
            environment("CARGO_TARGET_DIR", cargoTargetDir.get().asFile.absolutePath)
            commandLine(
                cargoBin, "ndk",
                "-t", "arm64-v8a",
                "-P", "26", // same API level as the crate's .cargo/config.toml linker
                "-o", out.absolutePath,
                "build", "--release", "--locked",
                // TOML literal strings (single quotes): Windows process creation drops embedded double quotes.
                "--config", "target.aarch64-linux-android.rustflags=['-C','link-arg=-Wl,--build-id=sha1']",
            )
        }
    }
}

val gnDownloadCrate = layout.projectDirectory.dir("src/main/cpp/gn-download/rust")

// Prefer an explicit CARGO, then rustup's default location, then whatever is on PATH.
val cargoExecutable: String = System.getenv("CARGO")
    ?: File(System.getProperty("user.home"), ".cargo/bin/cargo" + if (System.getProperty("os.name").startsWith("Windows")) ".exe" else "")
        .takeIf { it.isFile }?.absolutePath
    ?: "cargo"

// One task per picoXr variant, because AGP assigns each generated source directory itself. They
// share CARGO_TARGET_DIR, so after the first build the others only relink/copy.
androidComponents.onVariants(androidComponents.selector().withFlavor("androidApi" to "picoXr")) { variant ->
    val task = tasks.register<CargoNdkBuildTask>("buildGnDownload${variant.name.replaceFirstChar { it.uppercase() }}") {
        group = "xrgame"
        description = "Builds libgndownload.so (arm64-v8a) for ${variant.name} from src/main/cpp/gn-download/rust with cargo-ndk."
        crateDir.set(gnDownloadCrate)
        crateSources.from(fileTree(gnDownloadCrate) { exclude("target/**") })
        ndkDir.set(androidComponents.sdkComponents.ndkDirectory.map { it.asFile.absolutePath })
        cargo.set(cargoExecutable)
        cargoTargetDir.set(layout.buildDirectory.dir("cargo/gn-download"))
        buildInfo.set(layout.buildDirectory.file("outputs/gndownload/${variant.name}/BUILD_INFO.txt"))
    }
    variant.sources.jniLibs?.addGeneratedSourceDirectory(task, CargoNdkBuildTask::outputDir)
}
