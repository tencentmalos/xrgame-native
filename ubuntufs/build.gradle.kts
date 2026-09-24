plugins {
    alias(libs.plugins.android.dynamic.feature)
    alias(libs.plugins.jetbrains.kotlin.android)
}
android {
    namespace = "app.ubuntufs"
    compileSdk = 36

    defaultConfig {
        minSdk = 29
        // testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    flavorDimensions += "androidApi"
    productFlavors {
        create("legacy") {
            dimension = "androidApi"
        }
        create("legacyXr") {
            dimension = "androidApi"
        }
        create("modern") {
            dimension = "androidApi"
        }
        create("modernXr") {
            dimension = "androidApi"
        }
        create("picoXr") {
            dimension = "androidApi"
        }
    }

    buildTypes {
        create("release-signed") {
            initWith(getByName("release"))
        }
        create("release-gold") {
            initWith(getByName("release"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

// Mirrors the picoXr variant filter in app/build.gradle.kts.
androidComponents {
    beforeVariants(selector().withFlavor("androidApi" to "picoXr")) { variant ->
        if (variant.buildType == "release-signed" || variant.buildType == "release-gold") {
            variant.enable = false
        }
    }
}

dependencies {
    implementation(project(":app"))
    implementation(libs.androidx.core.ktx)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}
