plugins {
    kotlin("multiplatform")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.compose")
    id("dev.brahmkshatriya.compose")
}

val nativeVersion = "1.13.0-alpha09"

kotlin {
    linuxX64 {
        binaries.executable {
            entryPoint = "sample.main"
        }
    }

    sourceSets {
        linuxX64Main.dependencies {
            implementation(project(":"))
            implementation("dev.brahmkshatriya.compose.material3:material3:$nativeVersion")
            implementation("dev.brahmkshatriya.compose.desktop:desktop-native:$nativeVersion")
        }
    }
}

composeNativeApplication {
    applicationName.set("Wayland AppView Sample")
    packageName.set("dev.brahmkshatriya.wayland.appview.sample")
    executableName.set("wayland-appview-sample")
    packageVersion.set("0.1.0")
    description.set("Sample consumer for Wayland AppView")
    categories.set(listOf("Development", "Utility"))
}
