@file:OptIn(org.jetbrains.kotlin.gradle.ExperimentalKotlinGradlePluginApi::class)

import org.gradle.api.GradleException
import org.gradle.api.tasks.Exec
import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget
import java.io.File
import java.util.Locale

plugins {
    kotlin("multiplatform") version "2.3.20"
    id("org.jetbrains.kotlin.plugin.compose") version "2.3.20"
    id("org.jetbrains.compose") version "1.13.0-alpha01" apply false
    id("dev.brahmkshatriya.compose") version "1.13.0-alpha09" apply false
    id("com.vanniktech.maven.publish") version "0.37.0"
}

val nativeVersion = "1.13.0-alpha09"
val libraryVersion =
    providers.gradleProperty("VERSION_NAME")
        .orElse("0.1.1-SNAPSHOT")
        .get()

group = providers.gradleProperty("GROUP").orElse("dev.brahmkshatriya.wayland").get()
version = libraryVersion

val generatedProtocols = layout.buildDirectory.dir("generated/wayland-protocols")
val nativeSourceDir = layout.projectDirectory.dir("src/nativeInterop/cinterop")
val nativeIncludeDir = nativeSourceDir.dir("include")

data class LinuxToolchain(
    val cxx: List<String>,
    val cc: List<String>,
    val ar: List<String>,
)

private fun String.commandParts(): List<String> =
    trim().split(Regex("\\s+")).filter(String::isNotBlank)

private fun String.environmentStem(): String =
    replace(Regex("([a-z0-9])([A-Z])"), "$1_$2").uppercase(Locale.US)

fun executableOnPath(name: String): String? =
    System.getenv("PATH")
        ?.split(File.pathSeparator)
        ?.asSequence()
        ?.map { File(it, name) }
        ?.firstOrNull { it.isFile && it.canExecute() }
        ?.absolutePath

val konanDataDir =
    File(
        providers.gradleProperty("konan.data.dir").orNull
            ?: System.getenv("KONAN_DATA_DIR")
            ?: "${System.getProperty("user.home")}/.konan",
    )

fun bundledKonanToolchain(targetName: String): LinuxToolchain? {
    val spec =
        when (targetName) {
            "linuxX64" ->
                "x86_64-unknown-linux-gnu-gcc-8.3.0-glibc-2.19-kernel-4.9-2" to
                    "x86_64-unknown-linux-gnu"
            "linuxArm64" ->
                "aarch64-unknown-linux-gnu-gcc-8.3.0-glibc-2.25-kernel-4.9-2" to
                    "aarch64-unknown-linux-gnu"
            else -> return null
        }
    val bin = konanDataDir.resolve("dependencies/${spec.first}/bin")
    val cxx = bin.resolve("${spec.second}-g++")
    val cc = bin.resolve("${spec.second}-gcc")
    val ar = bin.resolve("${spec.second}-ar")
    return if (cxx.canExecute() && cc.canExecute() && ar.canExecute()) {
        LinuxToolchain(listOf(cxx.absolutePath), listOf(cc.absolutePath), listOf(ar.absolutePath))
    } else {
        null
    }
}

fun defaultLinuxToolchain(targetName: String): LinuxToolchain? {
    val hostOs = System.getProperty("os.name").lowercase()
    val hostArch = System.getProperty("os.arch").lowercase()
    if (!hostOs.contains("linux")) return bundledKonanToolchain(targetName)

    val hostIsArm64 = hostArch == "aarch64" || hostArch == "arm64"
    val nativeHost =
        (targetName == "linuxX64" && !hostIsArm64) ||
            (targetName == "linuxArm64" && hostIsArm64)
    if (nativeHost) {
        return LinuxToolchain(listOf("c++"), listOf("cc"), listOf("ar"))
    }

    val prefix = if (targetName == "linuxArm64") "aarch64-linux-gnu" else "x86_64-linux-gnu"
    val systemCxx = executableOnPath("$prefix-g++")
    val systemCc = executableOnPath("$prefix-gcc")
    val systemAr = executableOnPath("$prefix-ar")
    if (systemCxx != null && systemCc != null && systemAr != null) {
        return LinuxToolchain(listOf(systemCxx), listOf(systemCc), listOf(systemAr))
    }
    return bundledKonanToolchain(targetName)
}

fun protocolTask(
    name: String,
    mode: String,
    xml: String,
    output: String,
) = tasks.register<Exec>(name) {
    val out = generatedProtocols.map { it.file(output) }
    inputs.file(xml)
    outputs.file(out)
    doFirst { out.get().asFile.parentFile.mkdirs() }
    commandLine("wayland-scanner", mode, xml, out.get().asFile.absolutePath)
}

val generateXdgHeader = protocolTask(
    "generateXdgShellHeader",
    "server-header",
    "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml",
    "xdg-shell-server-protocol.h",
)
val generateXdgCode = protocolTask(
    "generateXdgShellCode",
    "private-code",
    "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml",
    "xdg-shell-protocol.c",
)
val generateDmabufHeader = protocolTask(
    "generateLinuxDmabufHeader",
    "server-header",
    "/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml",
    "linux-dmabuf-unstable-v1-server-protocol.h",
)
val generateDmabufCode = protocolTask(
    "generateLinuxDmabufCode",
    "private-code",
    "/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml",
    "linux-dmabuf-protocol.c",
)
val generateViewporterHeader = protocolTask(
    "generateViewporterHeader",
    "server-header",
    "/usr/share/wayland-protocols/stable/viewporter/viewporter.xml",
    "viewporter-server-protocol.h",
)
val generateViewporterCode = protocolTask(
    "generateViewporterCode",
    "private-code",
    "/usr/share/wayland-protocols/stable/viewporter/viewporter.xml",
    "viewporter-protocol.c",
)
val generateFractionalScaleHeader = protocolTask(
    "generateFractionalScaleHeader",
    "server-header",
    "/usr/share/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml",
    "fractional-scale-v1-server-protocol.h",
)
val generateFractionalScaleCode = protocolTask(
    "generateFractionalScaleCode",
    "private-code",
    "/usr/share/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml",
    "fractional-scale-v1-protocol.c",
)

val generateXdgDecorationHeader = protocolTask(
    "generateXdgDecorationHeader",
    "server-header",
    "/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",
    "xdg-decoration-unstable-v1-server-protocol.h",
)
val generateXdgDecorationCode = protocolTask(
    "generateXdgDecorationCode",
    "private-code",
    "/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",
    "xdg-decoration-unstable-v1-protocol.c",
)

val generateTextInputHeader = protocolTask(
    "generateTextInputHeader",
    "server-header",
    "/usr/share/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml",
    "text-input-unstable-v3-server-protocol.h",
)
val generateTextInputCode = protocolTask(
    "generateTextInputCode",
    "private-code",
    "/usr/share/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml",
    "text-input-unstable-v3-protocol.c",
)

val nativeHeaders = files(
    nativeSourceDir.file("appview.cpp"),
    nativeIncludeDir.file("appview.h"),
)

fun KotlinNativeTarget.configureAppViewInterop() {
    val targetName = name
    val suffix = targetName.replaceFirstChar(Char::uppercaseChar)
    val stem = targetName.environmentStem()
    val targetDir = layout.buildDirectory.dir("native-bridge/$targetName")
    val appViewObject = targetDir.map { it.file("appview.o") }
    val xdgObject = targetDir.map { it.file("xdg-shell-protocol.o") }
    val dmabufObject = targetDir.map { it.file("linux-dmabuf-protocol.o") }
    val viewporterObject = targetDir.map { it.file("viewporter-protocol.o") }
    val fractionalObject = targetDir.map { it.file("fractional-scale-v1-protocol.o") }
    val decorationObject = targetDir.map { it.file("xdg-decoration-unstable-v1-protocol.o") }
    val textInputObject = targetDir.map { it.file("text-input-unstable-v3-protocol.o") }
    val archive = targetDir.map { it.file("libwayland-appview.a") }

    val configuredCxx = providers.environmentVariable("WAYLAND_APPVIEW_${stem}_CXX")
    val configuredCc = providers.environmentVariable("WAYLAND_APPVIEW_${stem}_CC")
    val configuredAr = providers.environmentVariable("WAYLAND_APPVIEW_${stem}_AR")
    val configuredFlags = providers.environmentVariable("WAYLAND_APPVIEW_${stem}_CFLAGS")

    fun resolveToolchain(): LinuxToolchain {
        val defaults = defaultLinuxToolchain(targetName)
        val cxx = configuredCxx.orNull?.commandParts()?.takeIf { it.isNotEmpty() } ?: defaults?.cxx
        val cc = configuredCc.orNull?.commandParts()?.takeIf { it.isNotEmpty() } ?: defaults?.cc
        val ar = configuredAr.orNull?.commandParts()?.takeIf { it.isNotEmpty() } ?: defaults?.ar
        if (cxx == null || cc == null || ar == null) {
            throw GradleException(
                "No native toolchain found for $targetName. Set " +
                    "WAYLAND_APPVIEW_${stem}_CXX, WAYLAND_APPVIEW_${stem}_CC and " +
                    "WAYLAND_APPVIEW_${stem}_AR, or install a matching GNU cross toolchain.",
            )
        }
        return LinuxToolchain(cxx, cc, ar)
    }

    val commonNativeFlags = listOf(
        "-O2",
        "-fPIC",
        "-I${generatedProtocols.get().asFile.absolutePath}",
        "-I${nativeIncludeDir.asFile.absolutePath}",
        // Cross compilers should prefer their sysroot's libc headers, while still being able to
        // consume architecture-neutral Wayland/SDL/EGL development headers from the host.
        "-idirafter",
        "/usr/include",
    )

    val compileAppView = tasks.register<Exec>("compileAppView$suffix") {
        dependsOn(generateXdgHeader, generateDmabufHeader, generateViewporterHeader, generateFractionalScaleHeader, generateXdgDecorationHeader, generateTextInputHeader)
        inputs.files(nativeHeaders)
        outputs.file(appViewObject)
        doFirst {
            targetDir.get().asFile.mkdirs()
            val toolchain = resolveToolchain()
            commandLine(
                toolchain.cxx +
                    listOf("-std=gnu++2a", "-DGL_GLEXT_PROTOTYPES", "-D_GLIBCXX_USE_CXX11_ABI=0") +
                    commonNativeFlags +
                    configuredFlags.orNull?.commandParts().orEmpty() +
                    listOf(
                        "-c",
                        nativeSourceDir.file("appview.cpp").asFile.absolutePath,
                        "-o",
                        appViewObject.get().asFile.absolutePath,
                    ),
            )
        }
    }

    fun protocolCompileTask(
        taskName: String,
        generator: org.gradle.api.tasks.TaskProvider<Exec>,
        sourceName: String,
        output: org.gradle.api.provider.Provider<org.gradle.api.file.RegularFile>,
    ) = tasks.register<Exec>(taskName) {
        dependsOn(generator)
        val source = generatedProtocols.map { it.file(sourceName) }
        inputs.file(source)
        outputs.file(output)
        doFirst {
            targetDir.get().asFile.mkdirs()
            val toolchain = resolveToolchain()
            commandLine(
                toolchain.cc +
                    listOf("-std=c11") +
                    commonNativeFlags +
                    configuredFlags.orNull?.commandParts().orEmpty() +
                    listOf("-c", source.get().asFile.absolutePath, "-o", output.get().asFile.absolutePath),
            )
        }
    }

    val compileXdg = protocolCompileTask("compileXdgShell$suffix", generateXdgCode, "xdg-shell-protocol.c", xdgObject)
    val compileDmabuf = protocolCompileTask("compileLinuxDmabuf$suffix", generateDmabufCode, "linux-dmabuf-protocol.c", dmabufObject)
    val compileViewporter = protocolCompileTask("compileViewporter$suffix", generateViewporterCode, "viewporter-protocol.c", viewporterObject)
    val compileFractional = protocolCompileTask(
        "compileFractionalScale$suffix",
        generateFractionalScaleCode,
        "fractional-scale-v1-protocol.c",
        fractionalObject,
    )
    val compileDecoration = protocolCompileTask(
        "compileXdgDecoration$suffix",
        generateXdgDecorationCode,
        "xdg-decoration-unstable-v1-protocol.c",
        decorationObject,
    )
    val compileTextInput = protocolCompileTask(
        "compileTextInput$suffix",
        generateTextInputCode,
        "text-input-unstable-v3-protocol.c",
        textInputObject,
    )

    val archiveBridge = tasks.register<Exec>("archiveAppViewBridge$suffix") {
        dependsOn(compileAppView, compileXdg, compileDmabuf, compileViewporter, compileFractional, compileDecoration, compileTextInput)
        inputs.files(appViewObject, xdgObject, dmabufObject, viewporterObject, fractionalObject, decorationObject, textInputObject)
        outputs.file(archive)
        doFirst {
            val toolchain = resolveToolchain()
            commandLine(
                toolchain.ar +
                    listOf(
                        "rcs",
                        archive.get().asFile.absolutePath,
                        appViewObject.get().asFile.absolutePath,
                        xdgObject.get().asFile.absolutePath,
                        dmabufObject.get().asFile.absolutePath,
                        viewporterObject.get().asFile.absolutePath,
                        fractionalObject.get().asFile.absolutePath,
                        decorationObject.get().asFile.absolutePath,
                        textInputObject.get().asFile.absolutePath,
                    ),
            )
        }
    }

    compilations.getByName("main") {
        cinterops.create("appview") {
            defFile(nativeSourceDir.file("appview-$targetName.def").asFile)
            header(nativeIncludeDir.file("appview.h").asFile)
            includeDirs(nativeIncludeDir.asFile)
            packageName("dev.brahmkshatriya.wayland.appview.internal.cinterop")
            extraOpts(
                "-libraryPath",
                targetDir.get().asFile.absolutePath,
                "-staticLibrary",
                "libwayland-appview.a",
            )
        }
    }

    tasks.matching { it.name == "cinteropAppview$suffix" }.configureEach {
        dependsOn(archiveBridge)
        inputs.file(archive)
    }
}

kotlin {
    explicitApi()

    linuxX64 {
        configureAppViewInterop()
    }
    linuxArm64 {
        configureAppViewInterop()
    }

    sourceSets {
        linuxMain.dependencies {
            api("dev.brahmkshatriya.compose.ui:ui:$nativeVersion")
            implementation("dev.brahmkshatriya.compose.desktop:desktop-native:$nativeVersion")
        }
    }
}

mavenPublishing {
    pom {
        name.set("Wayland AppView")
        description.set("Embed Wayland-native Linux applications inside Compose Native UI.")
    }
}
