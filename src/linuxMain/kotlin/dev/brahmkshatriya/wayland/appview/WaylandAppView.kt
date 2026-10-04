@file:Suppress("OPT_IN_USAGE", "OPT_IN_USAGE_ERROR")
@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package dev.brahmkshatriya.wayland.appview

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.viewinterop.InteropKeyEvent
import androidx.compose.ui.viewinterop.InteropPointerEvent
import androidx.compose.ui.viewinterop.InteropPointerEventType
import androidx.compose.ui.viewinterop.NativeInteropView
import androidx.compose.ui.viewinterop.NativeView
import androidx.compose.ui.viewinterop.OpenGlInteropRenderTarget
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_child_pid
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_create
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_destroy
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_error
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_fullscreen_requested
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_is_mapped
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_key
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_launch
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_pointer_button
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_pointer_motion
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_render
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_scroll
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_set_focused
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_socket_name
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_terminate
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_text_input_preedit
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_text_input_delete_surrounding
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_text_input_commit
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_text_input_active
import dev.brahmkshatriya.wayland.appview.internal.cinterop.appview_title
import kotlinx.cinterop.toKString

/** A snapshot of the embedded Wayland application's current host-visible state. */
public data class WaylandAppViewStatus(
    public val socketName: String,
    public val processId: Int?,
    public val title: String,
    public val error: String?,
    public val isMapped: Boolean,
    public val fullscreenRequested: Boolean,
    public val textInputActive: Boolean = false,
)

/**
 * Owns one private Wayland compositor and the application embedded inside it.
 *
 * Use [rememberWaylandAppViewController] from Compose so the native compositor is closed when the
 * owning composition leaves the tree. When constructing this class manually, call [close].
 */
public class WaylandAppViewController public constructor() {
    private var handle = appview_create()

    /** The private `WAYLAND_DISPLAY` socket name. */
    public val socketName: String
        get() = handle?.let(::appview_socket_name)?.toKString().orEmpty()

    /** The latest xdg-toplevel title supplied by the embedded application. */
    public val title: String
        get() = handle?.let(::appview_title)?.toKString().orEmpty()

    /** Native compositor error text, or null when no error is currently reported. */
    public val error: String?
        get() = handle?.let(::appview_error)?.toKString()?.takeIf(String::isNotBlank)

    /** Child process id, or null when no launched child is currently running. */
    public val processId: Int?
        get() = handle?.let(::appview_child_pid)?.takeIf { it > 0 }

    /** Whether a root surface from the embedded client is currently mapped. */
    public val isMapped: Boolean
        get() = handle?.let(::appview_is_mapped) != 0

    /** Whether an embedded xdg-toplevel currently requests fullscreen. */
    public val fullscreenRequested: Boolean
        get() = handle?.let(::appview_fullscreen_requested) != 0

    /** Whether the focused embedded client currently has an active text-input-v3 session. */
    public val textInputActive: Boolean
        get() = handle?.let(::appview_text_input_active) != 0

    /** A point-in-time status snapshot suitable for logging or non-Compose callers. */
    public val status: WaylandAppViewStatus
        get() =
            WaylandAppViewStatus(
                socketName = socketName,
                processId = processId,
                title = title,
                error = error,
                isMapped = isMapped,
                fullscreenRequested = fullscreenRequested,
                textInputActive = textInputActive,
            )

    internal val nativeView: NativeInteropView =
        NativeInteropView.openGl(
            renderer = ::render,
            continuousRendering = true,
            pointerHandler = ::pointer,
            keyHandler = ::key,
            focusHandler = ::focus,
        )

    /**
     * Starts [command] on this controller's private Wayland display.
     *
     * If another child launched by this controller is running, it is terminated first. The command
     * is executed through `/bin/sh -lc`, allowing normal shell-style arguments and environment
     * assignments.
     */
    public fun launch(command: String): Boolean {
        val native = handle ?: return false
        val normalized = command.trim()
        return normalized.isNotEmpty() && appview_launch(native, normalized) != 0
    }

    /** Terminates the child launched by [launch], while keeping the compositor reusable. */
    public fun stop(): Unit {
        handle?.let(::appview_terminate)
    }

    /**
     * Sends IME preedit text to the focused embedded text input. Cursor offsets are UTF-8 byte
     * offsets, matching `zwp_text_input_v3.preedit_string`.
     */
    public fun updatePreedit(text: String, cursorBegin: Int, cursorEnd: Int): Unit {
        val native = handle ?: return
        appview_text_input_preedit(native, text, cursorBegin, cursorEnd)
    }

    /** Commits text through the focused embedded `zwp_text_input_v3` session. */
    public fun commitText(text: String): Unit {
        val native = handle ?: return
        appview_text_input_commit(native, text)
    }

    /** Requests deletion around the embedded text cursor, using UTF-8 byte lengths. */
    public fun deleteSurroundingText(beforeLength: UInt, afterLength: UInt): Unit {
        val native = handle ?: return
        appview_text_input_delete_surrounding(native, beforeLength, afterLength)
    }

    /** Requests another host render pass. Usually unnecessary because AppView renders continuously. */
    public fun requestRender(): Unit {
        nativeView.requestRender()
    }

    /** Releases the private compositor and all native resources owned by this controller. */
    public fun close(): Unit {
        val native = handle ?: return
        handle = null
        appview_destroy(native)
    }

    private fun render(target: OpenGlInteropRenderTarget): Boolean {
        val native = handle ?: return false
        return appview_render(
            native,
            target.framebuffer,
            target.width,
            target.height,
            target.density,
        ) != 0
    }

    private fun pointer(event: InteropPointerEvent): Boolean {
        val native = handle ?: return false
        when (event.type) {
            InteropPointerEventType.Move ->
                appview_pointer_motion(native, event.x.toInt(), event.y.toInt(), event.timeMillis.toUInt())

            InteropPointerEventType.Button ->
                appview_pointer_button(
                    native,
                    event.button,
                    if (event.pressed) 1 else 0,
                    event.timeMillis.toUInt(),
                )

            InteropPointerEventType.Scroll ->
                appview_scroll(
                    native,
                    event.scrollDeltaX.toDouble(),
                    event.scrollDeltaY.toDouble(),
                    event.timeMillis.toUInt(),
                )
        }
        return true
    }

    private fun key(event: InteropKeyEvent): Boolean {
        val native = handle ?: return false
        val evdev = composeKeyToEvdev(event.keyCode)
        if (evdev == 0) return false
        appview_key(native, evdev.toUInt(), if (event.pressed) 1 else 0, 0u)
        return true
    }

    private fun focus(focused: Boolean): Unit {
        handle?.let { appview_set_focused(it, if (focused) 1 else 0) }
    }
}

/** Creates a lifecycle-managed [WaylandAppViewController]. */
@Composable
public fun rememberWaylandAppViewController(): WaylandAppViewController {
    val controller = remember { WaylandAppViewController() }
    DisposableEffect(controller) {
        onDispose { controller.close() }
    }
    return controller
}

/**
 * Observes [WaylandAppViewController.status] on the Compose frame clock.
 *
 * Changes are only written into Compose state when the snapshot actually differs.
 */
@Composable
public fun rememberWaylandAppViewStatus(
    controller: WaylandAppViewController,
): State<WaylandAppViewStatus> {
    val state = remember(controller) { mutableStateOf(controller.status) }
    LaunchedEffect(controller) {
        while (true) {
            withFrameNanos {
                val next = controller.status
                if (next != state.value) state.value = next
            }
        }
    }
    return state
}

/**
 * Displays the Wayland application owned by [controller] inside the Compose hierarchy.
 *
 * [onFullscreenRequest] mirrors `xdg_toplevel.set_fullscreen`/`unset_fullscreen`. A desktop host
 * can use it to switch its outer [androidx.compose.ui.window.WindowState] placement and hide any
 * surrounding chrome while fullscreen is requested.
 */
@Composable
public fun WaylandAppView(
    controller: WaylandAppViewController,
    modifier: Modifier = Modifier,
    onFullscreenRequest: (Boolean) -> Unit = {},
    onStatusChange: (WaylandAppViewStatus) -> Unit = {},
): Unit {
    val status by rememberWaylandAppViewStatus(controller)
    var lastFullscreen by remember(controller) { mutableStateOf<Boolean?>(null) }
    var lastStatus by remember(controller) { mutableStateOf<WaylandAppViewStatus?>(null) }

    LaunchedEffect(status) {
        if (lastStatus != status) {
            lastStatus = status
            onStatusChange(status)
        }
        if (lastFullscreen != status.fullscreenRequested) {
            lastFullscreen = status.fullscreenRequested
            onFullscreenRequest(status.fullscreenRequested)
        }
    }

    NativeView(
        factory = { controller.nativeView },
        modifier = modifier,
    )
}

/**
 * Convenience overload that remembers a controller and launches [command] whenever it changes.
 * The returned controller can be used for stop/relaunch/status operations.
 */
@Composable
public fun WaylandAppView(
    command: String,
    modifier: Modifier = Modifier,
    onFullscreenRequest: (Boolean) -> Unit = {},
    onStatusChange: (WaylandAppViewStatus) -> Unit = {},
): WaylandAppViewController {
    val controller = rememberWaylandAppViewController()
    LaunchedEffect(controller, command) {
        if (command.isNotBlank()) controller.launch(command)
    }
    WaylandAppView(
        controller = controller,
        modifier = modifier,
        onFullscreenRequest = onFullscreenRequest,
        onStatusChange = onStatusChange,
    )
    return controller
}

private fun composeKeyToEvdev(code: Long): Int =
    when (code) {
        Key.Escape.keyCode -> 1
        Key.One.keyCode -> 2
        Key.Two.keyCode -> 3
        Key.Three.keyCode -> 4
        Key.Four.keyCode -> 5
        Key.Five.keyCode -> 6
        Key.Six.keyCode -> 7
        Key.Seven.keyCode -> 8
        Key.Eight.keyCode -> 9
        Key.Nine.keyCode -> 10
        Key.Zero.keyCode -> 11
        Key.Minus.keyCode -> 12
        Key.Equals.keyCode -> 13
        Key.Backspace.keyCode -> 14
        Key.Tab.keyCode -> 15
        Key.Q.keyCode -> 16
        Key.W.keyCode -> 17
        Key.E.keyCode -> 18
        Key.R.keyCode -> 19
        Key.T.keyCode -> 20
        Key.Y.keyCode -> 21
        Key.U.keyCode -> 22
        Key.I.keyCode -> 23
        Key.O.keyCode -> 24
        Key.P.keyCode -> 25
        Key.LeftBracket.keyCode -> 26
        Key.RightBracket.keyCode -> 27
        Key.Enter.keyCode -> 28
        Key.CtrlLeft.keyCode -> 29
        Key.A.keyCode -> 30
        Key.S.keyCode -> 31
        Key.D.keyCode -> 32
        Key.F.keyCode -> 33
        Key.G.keyCode -> 34
        Key.H.keyCode -> 35
        Key.J.keyCode -> 36
        Key.K.keyCode -> 37
        Key.L.keyCode -> 38
        Key.Semicolon.keyCode -> 39
        Key.Apostrophe.keyCode -> 40
        Key.Grave.keyCode -> 41
        Key.ShiftLeft.keyCode -> 42
        Key.Backslash.keyCode -> 43
        Key.Z.keyCode -> 44
        Key.X.keyCode -> 45
        Key.C.keyCode -> 46
        Key.V.keyCode -> 47
        Key.B.keyCode -> 48
        Key.N.keyCode -> 49
        Key.M.keyCode -> 50
        Key.Comma.keyCode -> 51
        Key.Period.keyCode -> 52
        Key.Slash.keyCode -> 53
        Key.ShiftRight.keyCode -> 54
        Key.AltLeft.keyCode -> 56
        Key.Spacebar.keyCode -> 57
        Key.CapsLock.keyCode -> 58
        Key.F1.keyCode -> 59
        Key.F2.keyCode -> 60
        Key.F3.keyCode -> 61
        Key.F4.keyCode -> 62
        Key.F5.keyCode -> 63
        Key.F6.keyCode -> 64
        Key.F7.keyCode -> 65
        Key.F8.keyCode -> 66
        Key.F9.keyCode -> 67
        Key.F10.keyCode -> 68
        Key.F11.keyCode -> 87
        Key.F12.keyCode -> 88
        Key.CtrlRight.keyCode -> 97
        Key.AltRight.keyCode -> 100
        Key.MoveHome.keyCode -> 102
        Key.DirectionUp.keyCode -> 103
        Key.PageUp.keyCode -> 104
        Key.DirectionLeft.keyCode -> 105
        Key.DirectionRight.keyCode -> 106
        Key.MoveEnd.keyCode -> 107
        Key.DirectionDown.keyCode -> 108
        Key.PageDown.keyCode -> 109
        Key.Insert.keyCode -> 110
        Key.Delete.keyCode -> 111
        Key.MetaLeft.keyCode -> 125
        Key.MetaRight.keyCode -> 126
        else -> 0
    }
