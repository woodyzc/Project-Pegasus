// Explicit, because in a Kotlin build script `java` resolves to the Java
// plugin extension rather than the package, so java.util.Properties does not.
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The Mapbox Navigation SDK is an opt-in dependency, and the app builds and
// tests without it.
//
// Not a preference: the SDK is served from Mapbox's own Maven repository,
// which refuses anonymous access. Resolving it needs a secret token with the
// DOWNLOADS:READ scope in ~/.gradle/gradle.properties, and a public token in
// the app. Without both, Gradle cannot see the artifact at all -- so making it
// unconditional would mean nobody could build this project, including its
// tests, without a Mapbox account.
//
// Everything that does not strictly need the SDK is therefore in src/main and
// covered by unit tests. src/mapbox holds the thin adapter that does.
//
//   ./gradlew -PwithMapbox=true assembleDebug
val withMapbox = providers.gradleProperty("withMapbox").orNull == "true"

// The PUBLIC Mapbox token (pk....), which the app sends with every routing
// request. Read from local.properties, which is not tracked by git -- a token
// in a tracked file is a token published to anyone who clones this.
//
// This is not the same token Gradle needs to DOWNLOAD the SDK. That one is
// secret (sk....), carries the DOWNLOADS:READ scope, and belongs in
// ~/.gradle/gradle.properties as MAPBOX_DOWNLOADS_TOKEN -- outside the repo
// entirely. See settings.gradle.kts.
//
// Empty when absent, so a build without Mapbox is unaffected and a build with
// it fails at runtime with a clear message rather than at compile time with an
// obscure one.
val mapboxAccessToken: String = run {
    val f = rootProject.file("local.properties")
    if (!f.exists()) return@run ""
    Properties().apply { f.inputStream().use { load(it) } }
        .getProperty("MAPBOX_ACCESS_TOKEN")
        .orEmpty()
}

android {
    namespace = "com.pegasus.tbt"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.pegasus.tbt"
        // 26: BluetoothGattCharacteristic write paths and notification
        // listener behaviour below this are different enough not to be worth
        // supporting for a personal head unit.
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"

        buildConfigField("String", "MAPBOX_ACCESS_TOKEN", "\"$mapboxAccessToken\"")
    }

    buildFeatures {
        buildConfig = true
    }

    sourceSets {
        getByName("main") {
            if (withMapbox) {
                java.srcDir("src/mapbox/java")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core-ktx:1.13.1")
    testImplementation("junit:junit:4.13.2")

    if (withMapbox) {
        // Pin the version. The Navigation SDK's API has changed shape across
        // majors -- v3 moved it under com.mapbox.navigationcore -- so a
        // floating version would silently stop compiling.
        implementation("com.mapbox.navigationcore:navigation:3.6.0")
        implementation("com.mapbox.navigationcore:android:3.6.0")
    }
}
