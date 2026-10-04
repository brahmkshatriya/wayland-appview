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
        maven("https://redirector.kotlinlang.org/maven/compose-dev")
    }
}

rootProject.name = "wayland-appview"
include(":sample")
