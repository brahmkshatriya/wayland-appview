@file:OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)

package sample

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.WindowPlacement
import androidx.compose.ui.window.WindowState
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import dev.brahmkshatriya.wayland.appview.WaylandAppView
import dev.brahmkshatriya.wayland.appview.rememberWaylandAppViewController
import dev.brahmkshatriya.wayland.appview.rememberWaylandAppViewStatus
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.toKString
import platform.posix.getenv

private const val DefaultChrome =
    "google-chrome-stable --ozone-platform=wayland --no-first-run " +
        "--user-data-dir=/tmp/wayland-appview-chrome about:blank"
private const val DefaultFirefox = "firefox --new-instance about:blank"

@OptIn(ExperimentalForeignApi::class)
@Composable
private fun App(windowState: WindowState) {
    val startupCommand = remember { getenv("APPVIEW_COMMAND")?.toKString()?.takeIf(String::isNotBlank) }
    val controller = rememberWaylandAppViewController()
    val status by rememberWaylandAppViewStatus(controller)
    var command by remember { mutableStateOf(startupCommand ?: DefaultChrome) }
    var fullscreen by remember { mutableStateOf(false) }

    androidx.compose.runtime.LaunchedEffect(controller, startupCommand) {
        startupCommand?.let(controller::launch)
    }

    MaterialTheme(colorScheme = darkColorScheme()) {
        Box(modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
            Box(
                modifier =
                    if (fullscreen) Modifier.fillMaxSize()
                    else Modifier.fillMaxSize().padding(top = 148.dp),
            ) {
                WaylandAppView(
                    controller = controller,
                    modifier = Modifier.fillMaxSize(),
                    onFullscreenRequest = { requested ->
                        fullscreen = requested
                        windowState.placement =
                            if (requested) WindowPlacement.Fullscreen else WindowPlacement.Floating
                    },
                )
                if (!fullscreen && status.processId == null) {
                    Text(
                        "Launch a Wayland-native application above.",
                        modifier = Modifier.align(Alignment.Center).padding(24.dp),
                    )
                }
            }

            if (!fullscreen) {
                Surface(
                    tonalElevation = 4.dp,
                    modifier = Modifier.fillMaxWidth().height(148.dp).align(Alignment.TopCenter),
                ) {
                    Column(
                        modifier = Modifier.fillMaxWidth().padding(12.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            OutlinedTextField(
                                value = command,
                                onValueChange = { command = it },
                                modifier = Modifier.fillMaxWidth(0.68f),
                                singleLine = true,
                                label = { Text("Wayland application command") },
                            )
                            Button(onClick = { controller.launch(command) }) { Text("Launch") }
                            OutlinedButton(onClick = controller::stop) { Text("Stop") }
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton(onClick = { command = DefaultChrome }) { Text("Chrome") }
                            OutlinedButton(onClick = { command = DefaultFirefox }) { Text("Firefox") }
                            Text(
                                text =
                                    status.error
                                        ?: status.title.takeIf(String::isNotBlank)
                                        ?: "Private compositor: ${status.socketName}",
                                modifier = Modifier.align(Alignment.CenterVertically),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
            }
        }
    }
}

public fun main(): Unit =
    application {
        val state = rememberWindowState()
        Window(
            onCloseRequest = ::exitApplication,
            title = "Wayland AppView",
            state = state,
        ) {
            App(state)
        }
    }
