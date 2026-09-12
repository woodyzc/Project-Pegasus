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

// The PUBLIC Mapbox token is deliberately NOT read here any more.
//
// It used to arrive as a buildConfigField from local.properties, which put it
// in the dex as a plain string -- verified with a canary token: unzip the APK,
// `strings classes3.dex`, and there it was. That made every APK built here a
// carrier of a live billing credential, and made rotating the token a rebuild
// and a reinstall.
//
// It is now entered on the phone and kept in the app's private preferences.
// See MapboxToken.kt. The only Mapbox credential this build still touches is
// the SECRET download token, which never reaches the app at all and lives in
// GRADLE_USER_HOME/gradle.properties -- see settings.gradle.kts.

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
