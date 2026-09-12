pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()

        // Mapbox serves the Navigation SDK from its own repository and refuses
        // anonymous access. The password is a SECRET token (sk....) with the
        // DOWNLOADS:READ scope, which is not the same as the public token
        // (pk....) the app itself uses at runtime -- you need both, and they
        // are created separately in the Mapbox account dashboard.
        //
        // Put the secret one in ~/.gradle/gradle.properties, NOT in this repo:
        //
        //     MAPBOX_DOWNLOADS_TOKEN=sk.ey...
        //
        // Declared unconditionally because a repository nobody authenticates
        // against is simply never consulted. Only -PwithMapbox=true actually
        // asks for an artifact from it.
        maven {
            url = uri("https://api.mapbox.com/downloads/v2/releases/maven")
            authentication { create<BasicAuthentication>("basic") }
            credentials {
                username = "mapbox"
                password = providers.gradleProperty("MAPBOX_DOWNLOADS_TOKEN").orNull.orEmpty()
            }
        }
    }
}

rootProject.name = "PegasusTBT"
include(":app")
