# Wayland AppView

Wayland AppView embeds Wayland-native Linux applications inside a Compose Native UI. It runs a small private Wayland compositor in-process, launches a client against that private display, and composites the client's surfaces into a Compose `NativeView`.

Supported Kotlin/Native targets:

- `linuxX64`
- `linuxArm64`

The compositor imports Linux DMA-BUF buffers through EGL/OpenGL when possible and falls back to `wl_shm`. It also forwards pointer/keyboard input, client cursor surfaces, fractional scaling, frame callbacks, popups/subsurfaces, and xdg-toplevel fullscreen requests.

## Dependency

```kotlin
dependencies {
    implementation("dev.brahmkshatriya.wayland:wayland-appview:<version>")
}
```

The library uses Compose Native `1.13.0-alpha09` from Maven Central and is built with Kotlin
`2.3.20`. Maven-local consumer smoke tests pass with both Kotlin `2.3.20` and `2.4.20`.

### Linux runtime/development libraries

Arch Linux / CachyOS:

```bash
sudo pacman -S sdl3 mesa libdrm wayland wayland-protocols libxkbcommon
```

Building Wayland AppView itself additionally requires a C/C++ toolchain and `wayland-scanner` (provided by `wayland`).

## Basic usage

The recommended API is a remembered controller plus `WaylandAppView`:

```kotlin
@Composable
fun Browser(windowState: WindowState) {
    val appView = rememberWaylandAppViewController()
    val status by rememberWaylandAppViewStatus(appView)
    var fullscreen by remember { mutableStateOf(false) }

    LaunchedEffect(appView) {
        appView.launch("firefox --new-instance about:blank")
    }

    WaylandAppView(
        controller = appView,
        modifier = Modifier.fillMaxSize(),
        onFullscreenRequest = { requested ->
            fullscreen = requested
            windowState.placement =
                if (requested) WindowPlacement.Fullscreen
                else WindowPlacement.Floating
        },
    )

    // status.processId
    // status.title
    // status.error
    // status.isMapped
    // status.fullscreenRequested
}
```

`rememberWaylandAppViewController()` owns the private compositor and releases it automatically when the composition is disposed. The controller also exposes:

```kotlin
appView.launch("google-chrome-stable --ozone-platform=wayland about:blank")
appView.stop()
appView.status
appView.socketName
appView.processId
appView.error
```

For simple cases there is a convenience overload that creates the controller and launches a command automatically:

```kotlin
val controller = WaylandAppView(
    command = "foot",
    modifier = Modifier.fillMaxSize(),
)
```

### Fullscreen

Embedded `xdg_toplevel.set_fullscreen` requests are surfaced through `onFullscreenRequest`. The library acknowledges the client with `XDG_TOPLEVEL_STATE_FULLSCREEN`; the host application decides how its outer Compose window and surrounding UI should react.

A typical desktop host sets `WindowState.placement` to `WindowPlacement.Fullscreen` and hides its toolbar while the callback is `true`. Maximize and minimize requests from the embedded client are intentionally ignored because AppView is an embedded surface rather than a desktop window manager.

## Sample

The `:sample` module is an actual consumer of the public library API. It includes Chrome/Firefox launch controls and makes only the AppView composable visible while an embedded application requests fullscreen.

Build it on x64 Linux:

```bash
./gradlew :sample:linkDebugExecutableLinuxX64
```

Run:

```bash
./sample/build/bin/linuxX64/debugExecutable/sample.kexe
```

Or launch an app immediately:

```bash
APPVIEW_COMMAND='foot' ./sample/build/bin/linuxX64/debugExecutable/sample.kexe
```

## Build and Maven Local

Build both published targets:

```bash
./gradlew compileKotlinLinuxX64 compileKotlinLinuxArm64
```

Publish a local snapshot without release signing:

```bash
./gradlew publishToMavenLocal -PRELEASE_SIGNING_ENABLED=false
```

The default local version is `0.1.0-SNAPSHOT`. Override it with the publishing plugin's
`VERSION_NAME` property:

```bash
./gradlew publishToMavenLocal \
    -PVERSION_NAME=0.2.0 \
    -PRELEASE_SIGNING_ENABLED=false
```

Published KMP variants are generated for the metadata module plus `linuxX64` and `linuxArm64`. Each Linux cinterop KLIB embeds the matching static AppView native bridge, so consumers do not need to build `appview.cpp` themselves.

### Native cross compilation

On Linux x64, `linuxArm64` can use a system GNU cross compiler when installed. If one is unavailable, the build can use Kotlin/Native's downloaded GNU AArch64 toolchain. Toolchains can also be overridden explicitly:

```text
WAYLAND_APPVIEW_LINUX_X64_CXX
WAYLAND_APPVIEW_LINUX_X64_CC
WAYLAND_APPVIEW_LINUX_X64_AR
WAYLAND_APPVIEW_LINUX_ARM64_CXX
WAYLAND_APPVIEW_LINUX_ARM64_CC
WAYLAND_APPVIEW_LINUX_ARM64_AR
```

Optional extra native compiler flags can be provided with `WAYLAND_APPVIEW_LINUX_X64_CFLAGS` and `WAYLAND_APPVIEW_LINUX_ARM64_CFLAGS`.

## Maven Central publishing

`.github/workflows/publish.yml` publishes on a tag or from a manual workflow dispatch. It only references these two GitHub Actions secrets:

- `GPG_SECRET_KEY_RING_BASE64`
- `GRADLE_PROPERTIES_CONTENT`

`GPG_SECRET_KEY_RING_BASE64` should be the base64-encoded secret GPG keyring. `GRADLE_PROPERTIES_CONTENT` should contain the Maven Central credentials and signing properties required by the Vanniktech Maven Publish plugin, for example:

```properties
mavenCentralUsername=...
mavenCentralPassword=...
signing.keyId=...
signing.password=...
```

The workflow writes the decoded keyring path into `signing.secretKeyRingFile`, so that property does not need to be hardcoded in the secret.

Manual publishes use the workflow's `version` input. Tag publishes use the tag name. Both are passed to Gradle as `VERSION_NAME`.

## Current limitations

Wayland AppView is an application embedder, not a complete desktop compositor. In particular:

- Clipboard/data-device objects exist, but clipboard contents are not yet bridged to the host desktop.
- Primary selection, text-input/IME, XDG activation, decoration-manager, and several optional desktop protocols are not implemented yet.
- Subsurface stacking (`place_above` / `place_below`) is not fully modeled.
- DMA-BUF release synchronization currently uses `glFinish()` rather than explicit GPU fences.
- X11-only applications require an Xwayland instance connected to the private compositor.
