#include "appview.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <SDL3/SDL.h>
#include <libdrm/drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <unordered_map>
#include <vector>

namespace {

struct AppView;
struct Surface;

struct DmaPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    bool set = false;
};

struct DmaBuffer {
    wl_resource* resource = nullptr;
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    uint32_t flags = 0;
    std::array<DmaPlane, 4> planes{};

    ~DmaBuffer() {
        for (auto& plane : planes) {
            if (plane.fd >= 0) close(plane.fd);
            plane.fd = -1;
        }
    }
};

struct DmaParams {
    AppView* view = nullptr;
    wl_resource* resource = nullptr;
    std::array<DmaPlane, 4> planes{};
    bool used = false;

    ~DmaParams() {
        for (auto& plane : planes) {
            if (plane.fd >= 0) close(plane.fd);
            plane.fd = -1;
        }
    }
};

struct Positioner {
    int width = 320;
    int height = 240;
    int anchorX = 0;
    int anchorY = 0;
    int anchorWidth = 1;
    int anchorHeight = 1;
    int offsetX = 0;
    int offsetY = 0;
    uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
    uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
};

struct ViewportState {
    bool sourceSet = false;
    double sourceX = 0.0;
    double sourceY = 0.0;
    double sourceWidth = 0.0;
    double sourceHeight = 0.0;
    bool destinationSet = false;
    int destinationWidth = -1;
    int destinationHeight = -1;
};

struct WindowGeometryState {
    bool set = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Surface {
    AppView* view = nullptr;
    wl_resource* resource = nullptr;
    wl_resource* pendingBuffer = nullptr;
    wl_resource* currentBuffer = nullptr;
    bool hasPendingAttach = false;
    wl_resource* cachedBuffer = nullptr;
    bool hasCachedAttach = false;
    bool hasCachedCommit = false;
    std::vector<wl_resource*> retiredBuffers;
    std::vector<wl_resource*> pendingFrameCallbacks;
    std::vector<wl_resource*> cachedFrameCallbacks;
    std::vector<wl_resource*> frameCallbacks;

    GLuint cachedShmTexture = 0;
    int cachedShmWidth = 0;
    int cachedShmHeight = 0;
    bool releaseShmAfterFrame = false;

    wl_resource* xdgSurface = nullptr;
    wl_resource* xdgToplevel = nullptr;
    wl_resource* xdgPopup = nullptr;
    wl_resource* subsurface = nullptr;
    wl_resource* viewport = nullptr;
    wl_resource* fractionalScale = nullptr;
    Surface* parent = nullptr;
    bool synchronized = true;

    int x = 0;
    int y = 0;
    int pendingX = 0;
    int pendingY = 0;
    bool hasPendingPosition = false;
    int bufferScale = 1;
    int pendingBufferScale = 1;
    int cachedBufferScale = 1;
    int transform = WL_OUTPUT_TRANSFORM_NORMAL;
    int pendingTransform = WL_OUTPUT_TRANSFORM_NORMAL;
    int cachedTransform = WL_OUTPUT_TRANSFORM_NORMAL;
    ViewportState viewportState;
    ViewportState pendingViewportState;
    ViewportState cachedViewportState;
    WindowGeometryState windowGeometry;
    WindowGeometryState pendingWindowGeometry;
    WindowGeometryState cachedWindowGeometry;
    bool mapped = false;
    bool dirty = true;
    bool enteredOutput = false;
    bool fullscreenRequested = false;
    bool cursorRole = false;
    std::vector<uint8_t> cursorPixels;
    int cursorPixelWidth = 0;
    int cursorPixelHeight = 0;
    int cursorPixelStride = 0;
    std::string title;
};

struct OutputBinding {
    wl_client* client = nullptr;
    wl_resource* resource = nullptr;
};

struct SeatBinding {
    wl_client* client = nullptr;
    wl_resource* seat = nullptr;
    wl_resource* pointer = nullptr;
    wl_resource* keyboard = nullptr;
};

struct GlState {
    GLint framebuffer = 0;
    GLint viewport[4] = {};
    GLint program = 0;
    GLint vao = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = 0;
    GLint texture = 0;
    GLboolean blend = GL_FALSE;
    GLint blendSrcRgb = GL_ONE;
    GLint blendDstRgb = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;
};

struct Renderer {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint textureUniform = -1;
    bool initialized = false;
};

struct AppView {
    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    std::string socketName;
    std::string error;
    std::string title = "Embedded application";
    std::string pendingCommand;
    pid_t childPid = -1;

    wl_global* compositorGlobal = nullptr;
    wl_global* subcompositorGlobal = nullptr;
    wl_global* xdgGlobal = nullptr;
    wl_global* outputGlobal = nullptr;
    wl_global* seatGlobal = nullptr;
    wl_global* dataDeviceGlobal = nullptr;
    wl_global* dmabufGlobal = nullptr;
    wl_global* viewporterGlobal = nullptr;
    wl_global* fractionalScaleGlobal = nullptr;

    std::vector<std::unique_ptr<Surface>> surfaces;
    std::vector<OutputBinding> outputs;
    std::vector<SeatBinding> seats;

    Surface* pointerFocus = nullptr;
    Surface* keyboardFocus = nullptr;
    Surface* cursorSurface = nullptr;
    int cursorHotspotX = 0;
    int cursorHotspotY = 0;
    SDL_Cursor* customCursor = nullptr;
    bool hostFocused = false;
    double pointerX = 0.0;
    double pointerY = 0.0;
    bool pointerWindowOriginKnown = false;
    double pointerWindowOriginX = 0.0;
    double pointerWindowOriginY = 0.0;
    uint32_t serial = 1;

    int hostWidth = 1280;
    int hostHeight = 720;
    float density = 1.0f;
    bool sceneDirty = true;

    xkb_context* xkbContext = nullptr;
    xkb_keymap* xkbKeymap = nullptr;
    xkb_state* xkbState = nullptr;

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> dmaFormats;
    Renderer renderer;
    bool loggedDmaImport = false;
    bool loggedDmaImportFailure = false;
    bool loggedShmUpload = false;
};

static struct wl_compositor_interface g_compositorImpl{};
static struct wl_surface_interface g_surfaceImpl{};
static struct wl_region_interface g_regionImpl{};
static struct wl_subcompositor_interface g_subcompositorImpl{};
static struct wl_subsurface_interface g_subsurfaceImpl{};
static struct wl_output_interface g_outputImpl{};
static struct wl_seat_interface g_seatImpl{};
static struct wl_pointer_interface g_pointerImpl{};
static struct wl_keyboard_interface g_keyboardImpl{};
static struct wl_buffer_interface g_dmaBufferImpl{};
static struct wl_data_device_manager_interface g_dataDeviceManagerImpl{};
static struct wp_viewporter_interface g_viewporterImpl{};
static struct wp_viewport_interface g_viewportImpl{};
static struct wp_fractional_scale_manager_v1_interface g_fractionalScaleManagerImpl{};
static struct wp_fractional_scale_v1_interface g_fractionalScaleImpl{};
static struct wl_data_source_interface g_dataSourceImpl{};
static struct wl_data_device_interface g_dataDeviceImpl{};
static struct xdg_wm_base_interface g_xdgWmBaseImpl{};
static struct xdg_positioner_interface g_positionerImpl{};
static struct xdg_surface_interface g_xdgSurfaceImpl{};
static struct xdg_toplevel_interface g_toplevelImpl{};
static struct xdg_popup_interface g_popupImpl{};
static struct zwp_linux_dmabuf_v1_interface g_dmabufImpl{};
static struct zwp_linux_buffer_params_v1_interface g_dmaParamsImpl{};
static bool g_interfacesInitialized = false;
static std::unordered_map<wl_resource*, DmaBuffer*> g_dmaBuffers;

struct BufferWatch {
    wl_listener destroyListener{};
    wl_resource* resource = nullptr;
};

static std::unordered_map<wl_resource*, BufferWatch*> g_bufferWatches;

static void bufferWatchDestroyed(wl_listener* listener, void*) {
    auto* watch = reinterpret_cast<BufferWatch*>(listener);
    if (!watch) return;
    g_bufferWatches.erase(watch->resource);
    delete watch;
}

static void watchBufferResource(wl_resource* resource) {
    if (!resource || g_bufferWatches.find(resource) != g_bufferWatches.end()) return;
    auto* watch = new BufferWatch();
    watch->resource = resource;
    watch->destroyListener.notify = bufferWatchDestroyed;
    wl_resource_add_destroy_listener(resource, &watch->destroyListener);
    g_bufferWatches[resource] = watch;
}

static bool bufferResourceAlive(wl_resource* resource) {
    return resource && g_bufferWatches.find(resource) != g_bufferWatches.end();
}

static void releaseBufferResource(wl_resource* resource) {
    if (bufferResourceAlive(resource)) wl_buffer_send_release(resource);
}

static uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

static uint32_t nextSerial(AppView* view) {
    return wl_display_next_serial(view->display);
}

static Surface* surfaceFromResource(wl_resource* resource) {
    return resource ? static_cast<Surface*>(wl_resource_get_user_data(resource)) : nullptr;
}

static DmaBuffer* dmaBufferFromResource(wl_resource* resource) {
    if (!resource) return nullptr;
    auto it = g_dmaBuffers.find(resource);
    return it == g_dmaBuffers.end() ? nullptr : it->second;
}

static void setError(AppView* view, const std::string& message) {
    if (view) view->error = message;
}

static SeatBinding* seatBindingForSeatResource(AppView* view, wl_resource* seatResource) {
    for (auto& binding : view->seats) {
        if (binding.seat == seatResource) return &binding;
    }
    return nullptr;
}

static OutputBinding* outputBindingForClient(AppView* view, wl_client* client) {
    for (auto& binding : view->outputs) {
        if (binding.client == client) return &binding;
    }
    return nullptr;
}

static std::pair<int, int> bufferSize(wl_resource* buffer) {
    if (!bufferResourceAlive(buffer)) return {0, 0};
    if (auto* dma = dmaBufferFromResource(buffer)) return {dma->width, dma->height};
    if (auto* shm = wl_shm_buffer_get(buffer)) {
        return {wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm)};
    }
    return {0, 0};
}

static std::pair<double, double> surfaceLogicalSize(const Surface* surface, int bw, int bh) {
    if (!surface || bw <= 0 || bh <= 0) return {0.0, 0.0};
    const auto& viewport = surface->viewportState;
    if (viewport.destinationSet) {
        return {static_cast<double>(viewport.destinationWidth),
                static_cast<double>(viewport.destinationHeight)};
    }
    if (viewport.sourceSet) {
        return {viewport.sourceWidth, viewport.sourceHeight};
    }
    const int scale = std::max(1, surface->bufferScale);
    return {static_cast<double>(bw) / scale, static_cast<double>(bh) / scale};
}

static void resetHostCursor(AppView* view) {
    if (!view) return;
    if (view->customCursor) {
        if (SDL_Cursor* defaultCursor = SDL_GetDefaultCursor()) SDL_SetCursor(defaultCursor);
        SDL_DestroyCursor(view->customCursor);
        view->customCursor = nullptr;
    }
    view->cursorSurface = nullptr;
    SDL_ShowCursor();
}

static bool cacheCursorPixels(Surface* surface) {
    if (!surface) return false;
    if (!bufferResourceAlive(surface->currentBuffer)) return !surface->cursorPixels.empty();
    wl_shm_buffer* shm = wl_shm_buffer_get(surface->currentBuffer);
    if (!shm) return !surface->cursorPixels.empty();

    const int width = wl_shm_buffer_get_width(shm);
    const int height = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (width <= 0 || height <= 0 || stride < width * 4 ||
        (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)) {
        return !surface->cursorPixels.empty();
    }

    const int packedStride = width * 4;
    surface->cursorPixels.resize(static_cast<size_t>(packedStride) * height);
    wl_shm_buffer_begin_access(shm);
    const auto* source = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
    if (!source) {
        wl_shm_buffer_end_access(shm);
        surface->cursorPixels.clear();
        return false;
    }
    for (int row = 0; row < height; ++row) {
        std::memcpy(surface->cursorPixels.data() + static_cast<size_t>(row) * packedStride,
                    source + static_cast<size_t>(row) * stride,
                    static_cast<size_t>(packedStride));
    }
    wl_shm_buffer_end_access(shm);

    auto* pixels = reinterpret_cast<uint32_t*>(surface->cursorPixels.data());
    const size_t count = static_cast<size_t>(width) * height;
    if (format == WL_SHM_FORMAT_XRGB8888) {
        for (size_t i = 0; i < count; ++i) pixels[i] |= 0xff000000u;
    } else {
        // Wayland ARGB8888 is premultiplied-alpha, while SDL color cursor
        // surfaces use straight alpha and premultiply again for Wayland.
        // Convert back to straight alpha here to avoid dark/soft AA edges.
        for (size_t i = 0; i < count; ++i) {
            const uint32_t p = pixels[i];
            const uint32_t a = (p >> 24) & 0xffu;
            if (a == 0u) {
                pixels[i] = 0u;
                continue;
            }
            if (a == 0xffu) continue;
            const auto unpremul = [a](uint32_t c) {
                return std::min(255u, (c * 255u + a / 2u) / a);
            };
            const uint32_t r = unpremul((p >> 16) & 0xffu);
            const uint32_t g = unpremul((p >> 8) & 0xffu);
            const uint32_t b = unpremul(p & 0xffu);
            pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    surface->cursorPixelWidth = width;
    surface->cursorPixelHeight = height;
    surface->cursorPixelStride = packedStride;

    // SDL_CreateColorCursor copies the bitmap, so the Wayland client can immediately reuse
    // its wl_shm buffer after we have cached the pixels.
    releaseBufferResource(surface->currentBuffer);
    surface->currentBuffer = nullptr;
    surface->mapped = true;
    return true;
}

static void applyHostCursor(AppView* view) {
    if (!view) return;
    Surface* surface = view->cursorSurface;
    if (!surface) {
        if (view->customCursor) {
            SDL_DestroyCursor(view->customCursor);
            view->customCursor = nullptr;
        }
        SDL_HideCursor();
        return;
    }

    cacheCursorPixels(surface);
    if (surface->cursorPixels.empty() || surface->cursorPixelWidth <= 0 ||
        surface->cursorPixelHeight <= 0 || surface->cursorPixelStride <= 0) {
        return;
    }

    const auto [logicalWidth, logicalHeight] =
        surfaceLogicalSize(surface, surface->cursorPixelWidth, surface->cursorPixelHeight);
    const int targetWidth = std::max(1, static_cast<int>(std::lround(logicalWidth)));
    const int targetHeight = std::max(1, static_cast<int>(std::lround(logicalHeight)));
    const int hotspotX = std::clamp(
        view->cursorHotspotX, 0,
        std::max(0, targetWidth - 1));
    const int hotspotY = std::clamp(
        view->cursorHotspotY, 0,
        std::max(0, targetHeight - 1));

    SDL_Surface* sourceBitmap = SDL_CreateSurfaceFrom(
        surface->cursorPixelWidth,
        surface->cursorPixelHeight,
        SDL_PIXELFORMAT_ARGB8888,
        surface->cursorPixels.data(),
        surface->cursorPixelStride);
    if (!sourceBitmap) return;
    SDL_Surface* bitmap = sourceBitmap;
    if (targetWidth != surface->cursorPixelWidth || targetHeight != surface->cursorPixelHeight) {
        bitmap = SDL_ScaleSurface(sourceBitmap, targetWidth, targetHeight, SDL_SCALEMODE_LINEAR);
        if (!bitmap) {
            SDL_DestroySurface(sourceBitmap);
            return;
        }
        // Keep the client's original high-resolution cursor as an alternate
        // image. SDL's Wayland backend will use it as the backing buffer and
        // set a viewport destination of targetWidth/targetHeight logical px,
        // which preserves sharpness on fractional-scale outputs.
        SDL_AddSurfaceAlternateImage(bitmap, sourceBitmap);
    }
    SDL_Cursor* cursor = SDL_CreateColorCursor(bitmap, hotspotX, hotspotY);
    if (bitmap != sourceBitmap) SDL_DestroySurface(bitmap);
    SDL_DestroySurface(sourceBitmap);
    if (!cursor) return;

    SDL_ShowCursor();
    SDL_SetCursor(cursor);
    if (view->customCursor) SDL_DestroyCursor(view->customCursor);
    view->customCursor = cursor;
}

static std::pair<int, int> absolutePosition(const Surface* surface) {
    if (!surface) return {0, 0};

    int x = surface->x;
    int y = surface->y;
    if (surface->parent) {
        auto [parentX, parentY] = absolutePosition(surface->parent);
        if (surface->xdgPopup && surface->parent->windowGeometry.set) {
            parentX += surface->parent->windowGeometry.x;
            parentY += surface->parent->windowGeometry.y;
        }
        x += parentX;
        y += parentY;
    }

    if ((surface->xdgToplevel || surface->xdgPopup) && surface->windowGeometry.set) {
        x -= surface->windowGeometry.x;
        y -= surface->windowGeometry.y;
    }
    return {x, y};
}

static bool surfaceVisible(const Surface* surface) {
    if (!surface || !surface->mapped || surface->cursorRole) return false;
    for (const Surface* parent = surface->parent; parent; parent = parent->parent) {
        if (!parent->mapped) return false;
    }
    return true;
}

static Surface* rootSurface(AppView* view) {
    for (auto& candidate : view->surfaces) {
        if (!candidate->cursorRole && candidate->mapped && candidate->xdgToplevel && candidate->parent == nullptr) {
            return candidate.get();
        }
    }
    for (auto& candidate : view->surfaces) {
        if (!candidate->cursorRole && candidate->mapped && candidate->parent == nullptr) return candidate.get();
    }
    return nullptr;
}

static Surface* hitTest(AppView* view, double x, double y) {
    for (auto it = view->surfaces.rbegin(); it != view->surfaces.rend(); ++it) {
        Surface* surface = it->get();
        if (!surfaceVisible(surface)) continue;
        auto [bw, bh] = bufferSize(surface->currentBuffer);
        if ((bw <= 0 || bh <= 0) && surface->cachedShmTexture != 0) {
            bw = surface->cachedShmWidth;
            bh = surface->cachedShmHeight;
        }
        if (bw <= 0 || bh <= 0) continue;
        const auto [width, height] = surfaceLogicalSize(surface, bw, bh);
        auto [sx, sy] = absolutePosition(surface);
        if (x >= sx && y >= sy && x < sx + width && y < sy + height) return surface;
    }
    return rootSurface(view);
}

static void sendOutputEnter(Surface* surface) {
    if (!surface || surface->enteredOutput || !surface->resource) return;
    auto* view = surface->view;
    auto* output = outputBindingForClient(view, wl_resource_get_client(surface->resource));
    if (!output || !output->resource) return;
    wl_surface_send_enter(surface->resource, output->resource);
    surface->enteredOutput = true;
}

static void sendToplevelConfigure(Surface* surface) {
    if (!surface || !surface->xdgSurface || !surface->xdgToplevel) return;
    auto* view = surface->view;
    const int width = std::max(1, static_cast<int>(std::lround(view->hostWidth / view->density)));
    const int height = std::max(1, static_cast<int>(std::lround(view->hostHeight / view->density)));
    wl_array states;
    wl_array_init(&states);
    if (view->hostFocused) {
        auto* state = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (state) *state = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    if (surface->fullscreenRequested) {
        auto* state = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (state) *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(surface->xdgToplevel, width, height, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(surface->xdgSurface, nextSerial(view));
}

static void sendPopupConfigure(Surface* surface, Positioner* positioner) {
    if (!surface || !surface->xdgSurface || !surface->xdgPopup) return;
    int x = positioner ? positioner->anchorX + positioner->offsetX : 0;
    int y = positioner ? positioner->anchorY + positioner->offsetY : 0;
    int width = positioner ? std::max(1, positioner->width) : 320;
    int height = positioner ? std::max(1, positioner->height) : 240;
    surface->x = x;
    surface->y = y;
    xdg_popup_send_configure(surface->xdgPopup, x, y, width, height);
    xdg_surface_send_configure(surface->xdgSurface, nextSerial(surface->view));
}

static void sendKeyboardModifiers(AppView* view, wl_resource* keyboard) {
    if (!view || !keyboard || !view->xkbState) return;
    const uint32_t depressed = xkb_state_serialize_mods(view->xkbState, XKB_STATE_MODS_DEPRESSED);
    const uint32_t latched = xkb_state_serialize_mods(view->xkbState, XKB_STATE_MODS_LATCHED);
    const uint32_t locked = xkb_state_serialize_mods(view->xkbState, XKB_STATE_MODS_LOCKED);
    const uint32_t group = xkb_state_serialize_layout(view->xkbState, XKB_STATE_LAYOUT_EFFECTIVE);
    wl_keyboard_send_modifiers(keyboard, nextSerial(view), depressed, latched, locked, group);
}

static int createKeymapFd(AppView* view, size_t* outSize) {
    if (!view->xkbKeymap) return -1;
    char* text = xkb_keymap_get_as_string(view->xkbKeymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text) return -1;
    const size_t size = std::strlen(text) + 1;
    char path[] = "/tmp/wayland-appview-keymap-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        free(text);
        return -1;
    }
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        free(text);
        return -1;
    }
    ssize_t written = pwrite(fd, text, size, 0);
    free(text);
    if (written != static_cast<ssize_t>(size)) {
        close(fd);
        return -1;
    }
    *outSize = size;
    return fd;
}

static GLuint compileShader(GLenum type, const char* source, std::string* error) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(1, length), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        if (error) *error = log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool initializeRenderer(AppView* view) {
    if (view->renderer.initialized) return true;
    static constexpr const char* kVertex = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)GLSL";
    static constexpr const char* kFragment = R"GLSL(
#version 330 core
in vec2 vTexCoord;
out vec4 outColor;
uniform sampler2D uTexture;
void main() {
    outColor = texture(uTexture, vTexCoord);
}
)GLSL";

    std::string error;
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertex, &error);
    if (!vs) {
        setError(view, "OpenGL vertex shader failed: " + error);
        return false;
    }
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragment, &error);
    if (!fs) {
        glDeleteShader(vs);
        setError(view, "OpenGL fragment shader failed: " + error);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(1, length), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        glDeleteProgram(program);
        setError(view, "OpenGL program link failed: " + log);
        return false;
    }

    glGenVertexArrays(1, &view->renderer.vao);
    glGenBuffers(1, &view->renderer.vbo);
    glBindVertexArray(view->renderer.vao);
    glBindBuffer(GL_ARRAY_BUFFER, view->renderer.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          reinterpret_cast<void*>(sizeof(float) * 2));
    glBindVertexArray(0);

    view->renderer.program = program;
    view->renderer.textureUniform = glGetUniformLocation(program, "uTexture");
    view->renderer.initialized = true;
    return true;
}

static void saveGlState(GlState* state) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->framebuffer);
    glGetIntegerv(GL_VIEWPORT, state->viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state->activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture);
    state->blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &state->blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &state->blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &state->blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &state->blendDstAlpha);
}

static void restoreGlState(const GlState& state) {
    if (state.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(state.blendSrcRgb, state.blendDstRgb,
                        state.blendSrcAlpha, state.blendDstAlpha);
    glUseProgram(state.program);
    glBindVertexArray(state.vao);
    glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glActiveTexture(state.activeTexture);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.framebuffer);
    glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
}

static bool initializeEgl(AppView* view) {
    if (view->eglDisplay != EGL_NO_DISPLAY) return true;
    view->eglDisplay = eglGetCurrentDisplay();
    if (view->eglDisplay == EGL_NO_DISPLAY) {
        setError(view, "No current EGL display. Run the Compose Native host on Wayland/OpenGL.");
        return false;
    }
    view->eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    view->eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    view->glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    view->eglQueryDmaBufFormatsEXT = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    view->eglQueryDmaBufModifiersEXT = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!view->eglCreateImageKHR || !view->eglDestroyImageKHR ||
        !view->glEGLImageTargetTexture2DOES) {
        setError(view, "EGL DMA-BUF import extensions are unavailable on the current GL context.");
        return false;
    }

    if (view->eglQueryDmaBufFormatsEXT && view->eglQueryDmaBufModifiersEXT) {
        EGLint count = 0;
        if (view->eglQueryDmaBufFormatsEXT(view->eglDisplay, 0, nullptr, &count) && count > 0) {
            std::vector<EGLint> formats(static_cast<size_t>(count));
            if (view->eglQueryDmaBufFormatsEXT(view->eglDisplay, count, formats.data(), &count)) {
                for (int i = 0; i < count; ++i) {
                    EGLint modifierCount = 0;
                    if (!view->eglQueryDmaBufModifiersEXT(
                            view->eglDisplay, formats[i], 0, nullptr, nullptr, &modifierCount) ||
                        modifierCount <= 0) {
                        view->dmaFormats.emplace_back(static_cast<uint32_t>(formats[i]),
                                                      DRM_FORMAT_MOD_INVALID);
                        continue;
                    }
                    std::vector<EGLuint64KHR> modifiers(static_cast<size_t>(modifierCount));
                    std::vector<EGLBoolean> externalOnly(static_cast<size_t>(modifierCount));
                    if (view->eglQueryDmaBufModifiersEXT(
                            view->eglDisplay, formats[i], modifierCount, modifiers.data(),
                            externalOnly.data(), &modifierCount)) {
                        for (int j = 0; j < modifierCount; ++j) {
                            if (!externalOnly[j]) {
                                view->dmaFormats.emplace_back(
                                    static_cast<uint32_t>(formats[i]), modifiers[j]);
                            }
                        }
                    }
                }
            }
        }
    }

    if (view->dmaFormats.empty()) {
        for (uint32_t format : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
                                DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888}) {
            view->dmaFormats.emplace_back(format, DRM_FORMAT_MOD_LINEAR);
            view->dmaFormats.emplace_back(format, DRM_FORMAT_MOD_INVALID);
        }
    }
    return initializeRenderer(view);
}

static bool importDmaTexture(AppView* view, DmaBuffer* dma, GLuint* texture, EGLImageKHR* image) {
    if (!view || !dma || !texture || !image) return false;
    std::vector<EGLint> attrs;
    attrs.reserve(48);
    attrs.push_back(EGL_WIDTH); attrs.push_back(dma->width);
    attrs.push_back(EGL_HEIGHT); attrs.push_back(dma->height);
    attrs.push_back(EGL_LINUX_DRM_FOURCC_EXT); attrs.push_back(static_cast<EGLint>(dma->format));

    static constexpr EGLint kFdAttr[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static constexpr EGLint kOffsetAttr[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static constexpr EGLint kPitchAttr[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static constexpr EGLint kModLoAttr[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static constexpr EGLint kModHiAttr[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    for (size_t i = 0; i < dma->planes.size(); ++i) {
        const auto& plane = dma->planes[i];
        if (!plane.set) continue;
        attrs.push_back(kFdAttr[i]); attrs.push_back(plane.fd);
        attrs.push_back(kOffsetAttr[i]); attrs.push_back(static_cast<EGLint>(plane.offset));
        attrs.push_back(kPitchAttr[i]); attrs.push_back(static_cast<EGLint>(plane.stride));
        if (plane.modifier != DRM_FORMAT_MOD_INVALID) {
            attrs.push_back(kModLoAttr[i]);
            attrs.push_back(static_cast<EGLint>(plane.modifier & 0xffffffffu));
            attrs.push_back(kModHiAttr[i]);
            attrs.push_back(static_cast<EGLint>((plane.modifier >> 32) & 0xffffffffu));
        }
    }
    attrs.push_back(EGL_NONE);

    *image = view->eglCreateImageKHR(view->eglDisplay, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (*image == EGL_NO_IMAGE_KHR) {
        if (!view->loggedDmaImportFailure) {
            const EGLint error = eglGetError();
            std::fprintf(stderr,
                         "Wayland AppView: EGL DMA-BUF import failed error=0x%04x "
                         "size=%dx%d fourcc=0x%08x planes=%d modifier=0x%016llx\n",
                         error, dma->width, dma->height, dma->format,
                         static_cast<int>(std::count_if(
                             dma->planes.begin(), dma->planes.end(),
                             [](const DmaPlane& plane) { return plane.set; })),
                         static_cast<unsigned long long>(dma->planes[0].modifier));
            view->loggedDmaImportFailure = true;
        }
        return false;
    }
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    view->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, *image);
    return true;
}

static bool uploadShmTexture(wl_resource* resource, GLuint* texture, bool* yInverted) {
    auto* shm = wl_shm_buffer_get(resource);
    if (!shm) return false;
    const int width = wl_shm_buffer_get_width(shm);
    const int height = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888 &&
        format != WL_SHM_FORMAT_ABGR8888 && format != WL_SHM_FORMAT_XBGR8888) {
        return false;
    }
    wl_shm_buffer_begin_access(shm);
    void* data = wl_shm_buffer_get_data(shm);
    if (*texture == 0) glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    const GLenum pixelFormat =
        (format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888)
            ? GL_RGBA
            : GL_BGRA;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 pixelFormat, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    wl_shm_buffer_end_access(shm);
    *yInverted = false;
    return true;
}

static void drawTexture(AppView* view, GLuint texture, int x, int y, int width, int height,
                        bool yInverted, float u0, float v0, float u1, float v1) {
    const float left = -1.0f + 2.0f * static_cast<float>(x) / view->hostWidth;
    const float right = -1.0f + 2.0f * static_cast<float>(x + width) / view->hostWidth;
    const float top = 1.0f - 2.0f * static_cast<float>(y) / view->hostHeight;
    const float bottom = 1.0f - 2.0f * static_cast<float>(y + height) / view->hostHeight;
    const float topV = yInverted ? v1 : v0;
    const float bottomV = yInverted ? v0 : v1;
    const float vertices[] = {
        left,  top,    u0, topV,
        left,  bottom, u0, bottomV,
        right, top,    u1, topV,
        right, bottom, u1, bottomV,
    };
    glUseProgram(view->renderer.program);
    glUniform1i(view->renderer.textureUniform, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(view->renderer.vao);
    glBindBuffer(GL_ARRAY_BUFFER, view->renderer.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static bool drawSurface(AppView* view, Surface* surface) {
    if (!surfaceVisible(surface)) return false;
    auto [bw, bh] = bufferSize(surface->currentBuffer);
    if ((bw <= 0 || bh <= 0) && surface->cachedShmTexture != 0) {
        bw = surface->cachedShmWidth;
        bh = surface->cachedShmHeight;
    }
    if (bw <= 0 || bh <= 0) return false;
    auto [sx, sy] = absolutePosition(surface);
    const auto [logicalWidth, logicalHeight] = surfaceLogicalSize(surface, bw, bh);
    const int x = static_cast<int>(std::lround(sx * view->density));
    const int y = static_cast<int>(std::lround(sy * view->density));
    const int width = std::max(1, static_cast<int>(std::lround(logicalWidth * view->density)));
    const int height = std::max(1, static_cast<int>(std::lround(logicalHeight * view->density)));

    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    if (surface->viewportState.sourceSet) {
        const double scale = std::max(1, surface->bufferScale);
        u0 = static_cast<float>((surface->viewportState.sourceX * scale) / bw);
        v0 = static_cast<float>((surface->viewportState.sourceY * scale) / bh);
        u1 = static_cast<float>(((surface->viewportState.sourceX + surface->viewportState.sourceWidth) * scale) / bw);
        v1 = static_cast<float>(((surface->viewportState.sourceY + surface->viewportState.sourceHeight) * scale) / bh);
    }

    GLuint texture = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    bool yInverted = false;
    bool ok = false;
    if (auto* dma = dmaBufferFromResource(surface->currentBuffer)) {
        yInverted = (dma->flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT) != 0;
        ok = importDmaTexture(view, dma, &texture, &image);
        if (ok && !view->loggedDmaImport) {
            const uint64_t modifier = dma->planes[0].modifier;
            std::fprintf(stderr,
                         "Wayland AppView: DMA-BUF imported %dx%d fourcc=0x%08x modifier=0x%016llx\n",
                         dma->width, dma->height, dma->format,
                         static_cast<unsigned long long>(modifier));
            view->loggedDmaImport = true;
        }
    } else if (bufferResourceAlive(surface->currentBuffer) && wl_shm_buffer_get(surface->currentBuffer)) {
        texture = surface->cachedShmTexture;
        ok = uploadShmTexture(surface->currentBuffer, &texture, &yInverted);
        if (ok) {
            surface->cachedShmTexture = texture;
            surface->cachedShmWidth = bw;
            surface->cachedShmHeight = bh;
            surface->releaseShmAfterFrame = true;
            if (!view->loggedShmUpload) {
                std::fprintf(stderr, "Wayland AppView: wl_shm fallback uploaded %dx%d\n", bw, bh);
                view->loggedShmUpload = true;
            }
        }
    } else if (surface->cachedShmTexture != 0) {
        texture = surface->cachedShmTexture;
        yInverted = false;
        ok = true;
    }
    if (!ok) return false;
    drawTexture(view, texture, x, y, width, height, yInverted, u0, v0, u1, v1);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (texture && texture != surface->cachedShmTexture) glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) view->eglDestroyImageKHR(view->eglDisplay, image);
    return true;
}

static void finishFrames(AppView* view) {
    const uint32_t time = nowMs();
    for (auto& surfacePtr : view->surfaces) {
        auto* surface = surfacePtr.get();
        for (wl_resource* callback : surface->frameCallbacks) {
            wl_callback_send_done(callback, time);
            wl_resource_destroy(callback);
        }
        surface->frameCallbacks.clear();
        for (wl_resource* buffer : surface->retiredBuffers) {
            releaseBufferResource(buffer);
        }
        surface->retiredBuffers.clear();
        if (surface->releaseShmAfterFrame && surface->currentBuffer) {
            releaseBufferResource(surface->currentBuffer);
            surface->currentBuffer = nullptr;
            surface->releaseShmAfterFrame = false;
        }
        surface->dirty = false;
    }
}

static bool renderScene(AppView* view, int framebuffer) {
    GlState state;
    saveGlState(&state);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
    glViewport(0, 0, view->hostWidth, view->hostHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    bool drew = false;
    for (auto& surface : view->surfaces) drew = drawSurface(view, surface.get()) || drew;
    glFinish();
    finishFrames(view);
    // Frame callbacks and wl_buffer.release events are generated by finishFrames().
    // Flush them immediately so clients can submit their next frame before the next
    // compositor tick. Delaying this flush until the following appview_render()
    // adds a full frame of protocol latency and effectively halves client FPS.
    wl_display_flush_clients(view->display);
    view->sceneDirty = false;
    restoreGlState(state);
    // Compose Native's OpenGL interop layer clears its backing texture in
    // kgl_layer_prepare() before invoking us. Even when Wayland has no new
    // commit, we must repaint the last complete scene or Compose will sample
    // that freshly-cleared texture for this frame, which presents as flicker.
    // The clear above also modifies the target when there are no mapped
    // surfaces yet, so a completed render pass is always a successful render.
    return true;
}

static void updateKeyboardFocus(AppView* view, Surface* target) {
    if (view->keyboardFocus == target) return;
    Surface* old = view->keyboardFocus;
    view->keyboardFocus = target;
    if (old && old->resource) {
        wl_client* client = wl_resource_get_client(old->resource);
        for (auto& seat : view->seats) {
            if (seat.client == client && seat.keyboard) {
                wl_keyboard_send_leave(seat.keyboard, nextSerial(view), old->resource);
            }
        }
    }
    if (target && target->resource) {
        wl_client* client = wl_resource_get_client(target->resource);
        for (auto& seat : view->seats) {
            if (seat.client == client && seat.keyboard) {
            wl_array keys;
            wl_array_init(&keys);
                wl_keyboard_send_enter(seat.keyboard, nextSerial(view), target->resource, &keys);
            wl_array_release(&keys);
                sendKeyboardModifiers(view, seat.keyboard);
            }
        }
    }
}

static void updatePointerFocus(AppView* view, Surface* target, uint32_t time) {
    if (view->pointerFocus == target) return;
    Surface* old = view->pointerFocus;
    view->pointerFocus = target;
    if (old && old->resource) {
        wl_client* client = wl_resource_get_client(old->resource);
        for (auto& seat : view->seats) {
            if (seat.client == client && seat.pointer) {
                wl_pointer_send_leave(seat.pointer, nextSerial(view), old->resource);
                if (wl_resource_get_version(seat.pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(seat.pointer);
            }
        }
    }
    if (target && target->resource) {
        auto [x, y] = absolutePosition(target);
        wl_client* client = wl_resource_get_client(target->resource);
        for (auto& seat : view->seats) {
            if (seat.client == client && seat.pointer) {
                wl_pointer_send_enter(seat.pointer, nextSerial(view), target->resource,
                                  wl_fixed_from_double(view->pointerX - x),
                                  wl_fixed_from_double(view->pointerY - y));
                if (wl_resource_get_version(seat.pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(seat.pointer);
            }
        }
    }
    (void)time;
}

static bool hostPointerInsideAppView(AppView* view) {
    if (!view || !view->pointerWindowOriginKnown) return true;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    const double x = static_cast<double>(mouseX) * view->density;
    const double y = static_cast<double>(mouseY) * view->density;
    return x >= view->pointerWindowOriginX &&
           y >= view->pointerWindowOriginY &&
           x < view->pointerWindowOriginX + view->hostWidth &&
           y < view->pointerWindowOriginY + view->hostHeight;
}

static void leaveAppViewPointer(AppView* view) {
    if (!view) return;
    updatePointerFocus(view, nullptr, nowMs());
    resetHostCursor(view);
    if (view->display) wl_display_flush_clients(view->display);
}

static bool spawnPendingChild(AppView* view) {
    if (!view || view->pendingCommand.empty() || view->childPid > 0) return true;
    const std::string command = std::exchange(view->pendingCommand, {});
    pid_t pid = fork();
    if (pid < 0) {
        setError(view, std::string("fork failed: ") + std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", view->socketName.c_str(), 1);
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);
        execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    view->childPid = pid;
    return true;
}

// ---- wl_region ------------------------------------------------------------

static void regionDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void regionAdd(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static void regionSubtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

// ---- wl_surface -----------------------------------------------------------

static void destroySurfaceResource(wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    auto* view = surface->view;
    if (view->pointerFocus == surface) view->pointerFocus = nullptr;
    if (view->keyboardFocus == surface) view->keyboardFocus = nullptr;
    if (view->cursorSurface == surface) resetHostCursor(view);
    if (surface->viewport) {
        wl_resource_set_user_data(surface->viewport, nullptr);
        surface->viewport = nullptr;
    }
    if (surface->fractionalScale) {
        wl_resource_set_user_data(surface->fractionalScale, nullptr);
        surface->fractionalScale = nullptr;
    }
    surface->resource = nullptr;
    surface->currentBuffer = nullptr;
    surface->pendingBuffer = nullptr;
    surface->hasPendingAttach = false;
    surface->mapped = false;
    if (surface->cachedShmTexture) {
        glDeleteTextures(1, &surface->cachedShmTexture);
        surface->cachedShmTexture = 0;
    }
    view->sceneDirty = true;
}

static void surfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void surfaceAttach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    if (auto* surface = surfaceFromResource(resource)) {
        watchBufferResource(buffer);
        surface->pendingBuffer = buffer;
        surface->hasPendingAttach = true;
    }
}

static void surfaceDamage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

static void surfaceFrame(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    wl_resource* callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (callback) surface->pendingFrameCallbacks.push_back(callback);
}

static void surfaceSetOpaque(wl_client*, wl_resource*, wl_resource*) {}
static void surfaceSetInput(wl_client*, wl_resource*, wl_resource*) {}

static bool isEffectivelySynchronized(const Surface* surface) {
    if (!surface || !surface->subsurface || !surface->parent) return false;
    if (surface->synchronized) return true;
    return isEffectivelySynchronized(surface->parent);
}

static void moveCallbacks(std::vector<wl_resource*>& from, std::vector<wl_resource*>& to) {
    if (from.empty()) return;
    to.insert(to.end(), from.begin(), from.end());
    from.clear();
}

static void applyAttachedBuffer(Surface* surface, wl_resource* buffer, bool hasAttach) {
    if (!surface || !hasAttach) return;
    if (buffer != surface->currentBuffer && surface->currentBuffer) {
        surface->retiredBuffers.push_back(surface->currentBuffer);
    }
    surface->currentBuffer = buffer;
    surface->mapped = surface->currentBuffer != nullptr || surface->cachedShmTexture != 0;
    if (!buffer) {
        surface->mapped = false;
        if (surface->cachedShmTexture) {
            glDeleteTextures(1, &surface->cachedShmTexture);
            surface->cachedShmTexture = 0;
            surface->cachedShmWidth = 0;
            surface->cachedShmHeight = 0;
        }
    }
}

static void applyPendingSurfaceState(Surface* surface) {
    if (!surface) return;
    applyAttachedBuffer(surface, surface->pendingBuffer, surface->hasPendingAttach);
    surface->pendingBuffer = nullptr;
    surface->hasPendingAttach = false;
    surface->bufferScale = std::max(1, surface->pendingBufferScale);
    surface->transform = surface->pendingTransform;
    surface->viewportState = surface->pendingViewportState;
    surface->windowGeometry = surface->pendingWindowGeometry;
    moveCallbacks(surface->pendingFrameCallbacks, surface->frameCallbacks);
    surface->dirty = true;
    if (surface->mapped) sendOutputEnter(surface);
}

static void cachePendingSurfaceState(Surface* surface) {
    if (!surface) return;
    if (surface->hasPendingAttach) {
        if (surface->hasCachedAttach && surface->cachedBuffer &&
            surface->cachedBuffer != surface->pendingBuffer) {
            // A newer synchronized content update superseded a buffer which never became visible.
            releaseBufferResource(surface->cachedBuffer);
        }
        surface->cachedBuffer = surface->pendingBuffer;
        surface->hasCachedAttach = true;
        surface->pendingBuffer = nullptr;
        surface->hasPendingAttach = false;
    }
    surface->cachedBufferScale = std::max(1, surface->pendingBufferScale);
    surface->cachedTransform = surface->pendingTransform;
    surface->cachedViewportState = surface->pendingViewportState;
    surface->cachedWindowGeometry = surface->pendingWindowGeometry;
    moveCallbacks(surface->pendingFrameCallbacks, surface->cachedFrameCallbacks);
    surface->hasCachedCommit = true;
}

static void applyCachedSurfaceState(Surface* surface) {
    if (!surface || !surface->hasCachedCommit) return;
    applyAttachedBuffer(surface, surface->cachedBuffer, surface->hasCachedAttach);
    surface->cachedBuffer = nullptr;
    surface->hasCachedAttach = false;
    surface->bufferScale = std::max(1, surface->cachedBufferScale);
    surface->transform = surface->cachedTransform;
    surface->viewportState = surface->cachedViewportState;
    surface->windowGeometry = surface->cachedWindowGeometry;
    moveCallbacks(surface->cachedFrameCallbacks, surface->frameCallbacks);
    surface->hasCachedCommit = false;
    surface->dirty = true;
    if (surface->mapped) sendOutputEnter(surface);
}

static bool applySynchronizedChildren(Surface* parent) {
    if (!parent || !parent->view) return false;
    bool changed = false;
    for (auto& childPtr : parent->view->surfaces) {
        Surface* child = childPtr.get();
        if (!child || child->parent != parent || !child->subsurface) continue;

        // wl_subsurface.set_position is double-buffered state on the parent surface.
        if (child->hasPendingPosition) {
            child->x = child->pendingX;
            child->y = child->pendingY;
            child->hasPendingPosition = false;
            changed = true;
        }

        if (!child->hasCachedCommit) continue;
        applyCachedSurfaceState(child);
        changed = true;
        // A synchronized child's content update carries dependencies on its synchronized children.
        changed = applySynchronizedChildren(child) || changed;
    }
    return changed;
}

static void surfaceCommit(wl_client*, wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    if (isEffectivelySynchronized(surface)) {
        cachePendingSurfaceState(surface);
        return;
    }

    applyPendingSurfaceState(surface);
    applySynchronizedChildren(surface);
    if (surface->cursorRole && surface->view->cursorSurface == surface) {
        applyHostCursor(surface->view);
    }
    surface->view->sceneDirty = true;
}

static void surfaceSetBufferTransform(wl_client*, wl_resource* resource, int32_t transform) {
    if (auto* surface = surfaceFromResource(resource)) surface->pendingTransform = transform;
}

static void surfaceSetBufferScale(wl_client*, wl_resource* resource, int32_t scale) {
    if (auto* surface = surfaceFromResource(resource)) surface->pendingBufferScale = std::max(1, scale);
}

static void surfaceDamageBuffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
static void surfaceOffset(wl_client*, wl_resource*, int32_t, int32_t) {}

// ---- wl_compositor --------------------------------------------------------

static void compositorCreateSurface(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    auto surface = std::make_unique<Surface>();
    surface->view = view;
    surface->resource = wl_resource_create(client, &wl_surface_interface, 5, id);
    if (!surface->resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->resource, &g_surfaceImpl, surface.get(), destroySurfaceResource);
    view->surfaces.push_back(std::move(surface));
}

static void compositorCreateRegion(wl_client* client, wl_resource*, uint32_t id) {
    wl_resource* region = wl_resource_create(client, &wl_region_interface, 1, id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &g_regionImpl, nullptr, nullptr);
}

static void compositorRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void bindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface, std::min(version, 5u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_compositorImpl, data, nullptr);
}

// ---- wl_subcompositor -----------------------------------------------------

static void subsurfaceResourceDestroyed(wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    surface->subsurface = nullptr;
    surface->parent = nullptr;
    surface->mapped = false;
    surface->view->sceneDirty = true;
}

static void subsurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void subsurfaceSetPosition(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    if (auto* surface = surfaceFromResource(resource)) {
        surface->pendingX = x;
        surface->pendingY = y;
        surface->hasPendingPosition = true;
    }
}
static void subsurfacePlaceAbove(wl_client*, wl_resource*, wl_resource*) {}
static void subsurfacePlaceBelow(wl_client*, wl_resource*, wl_resource*) {}
static void subsurfaceSetSync(wl_client*, wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) surface->synchronized = true;
}
static void subsurfaceSetDesync(wl_client*, wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    const bool wasEffectivelySynchronized = isEffectivelySynchronized(surface);
    surface->synchronized = false;
    const bool isNowEffectivelySynchronized = isEffectivelySynchronized(surface);
    if (wasEffectivelySynchronized && !isNowEffectivelySynchronized && surface->hasCachedCommit) {
        applyCachedSurfaceState(surface);
        applySynchronizedChildren(surface);
        surface->view->sceneDirty = true;
    }
}

static void subcompositorDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void subcompositorGetSubsurface(wl_client* client, wl_resource*, uint32_t id,
                                       wl_resource* surfaceResource, wl_resource* parentResource) {
    auto* surface = surfaceFromResource(surfaceResource);
    auto* parent = surfaceFromResource(parentResource);
    if (!surface || !parent) return;
    surface->parent = parent;
    surface->subsurface = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    if (!surface->subsurface) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->subsurface, &g_subsurfaceImpl, surface, subsurfaceResourceDestroyed);
}

static void bindSubcompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_subcompositor_interface, std::min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_subcompositorImpl, data, nullptr);
}

// ---- viewporter / fractional scale --------------------------------------

static void viewportResourceDestroyed(wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (surface && surface->viewport == resource) surface->viewport = nullptr;
}

static void viewportDestroy(wl_client*, wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) {
        surface->pendingViewportState = ViewportState{};
    }
    wl_resource_destroy(resource);
}

static void viewportSetSource(wl_client*, wl_resource* resource, wl_fixed_t x, wl_fixed_t y,
                              wl_fixed_t width, wl_fixed_t height) {
    auto* surface = surfaceFromResource(resource);
    if (!surface || !surface->resource) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "wl_surface was destroyed");
        return;
    }
    const double dx = wl_fixed_to_double(x);
    const double dy = wl_fixed_to_double(y);
    const double dw = wl_fixed_to_double(width);
    const double dh = wl_fixed_to_double(height);
    if (dx == -1.0 && dy == -1.0 && dw == -1.0 && dh == -1.0) {
        surface->pendingViewportState.sourceSet = false;
        return;
    }
    if (dx < 0.0 || dy < 0.0 || dw <= 0.0 || dh <= 0.0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid viewport source rectangle");
        return;
    }
    auto& state = surface->pendingViewportState;
    state.sourceSet = true;
    state.sourceX = dx;
    state.sourceY = dy;
    state.sourceWidth = dw;
    state.sourceHeight = dh;
}

static void viewportSetDestination(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    auto* surface = surfaceFromResource(resource);
    if (!surface || !surface->resource) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "wl_surface was destroyed");
        return;
    }
    if (width == -1 && height == -1) {
        surface->pendingViewportState.destinationSet = false;
        surface->pendingViewportState.destinationWidth = -1;
        surface->pendingViewportState.destinationHeight = -1;
        return;
    }
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid viewport destination size");
        return;
    }
    auto& state = surface->pendingViewportState;
    state.destinationSet = true;
    state.destinationWidth = width;
    state.destinationHeight = height;
}

static void viewporterDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void viewporterGetViewport(wl_client* client, wl_resource* manager, uint32_t id,
                                  wl_resource* surfaceResource) {
    auto* surface = surfaceFromResource(surfaceResource);
    if (!surface) return;
    if (surface->viewport) {
        wl_resource_post_error(manager, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "wl_surface already has a viewport");
        return;
    }
    wl_resource* resource = wl_resource_create(client, &wp_viewport_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->viewport = resource;
    wl_resource_set_implementation(resource, &g_viewportImpl, surface, viewportResourceDestroyed);
}

static void bindViewporter(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_viewporter_interface, std::min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_viewporterImpl, data, nullptr);
}

static uint32_t preferredFractionalScale(const AppView* view) {
    return std::max(1u, static_cast<uint32_t>(std::lround(std::max(0.1f, view->density) * 120.0f)));
}

static void fractionalScaleResourceDestroyed(wl_resource* resource) {
    auto* surface = surfaceFromResource(resource);
    if (surface && surface->fractionalScale == resource) surface->fractionalScale = nullptr;
}

static void fractionalScaleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void fractionalScaleManagerDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void fractionalScaleManagerGet(wl_client* client, wl_resource* manager, uint32_t id,
                                      wl_resource* surfaceResource) {
    auto* surface = surfaceFromResource(surfaceResource);
    if (!surface) return;
    if (surface->fractionalScale) {
        wl_resource_post_error(manager, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                               "wl_surface already has fractional scale state");
        return;
    }
    wl_resource* resource = wl_resource_create(client, &wp_fractional_scale_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->fractionalScale = resource;
    wl_resource_set_implementation(resource, &g_fractionalScaleImpl, surface,
                                   fractionalScaleResourceDestroyed);
    wp_fractional_scale_v1_send_preferred_scale(resource, preferredFractionalScale(surface->view));
}

static void bindFractionalScaleManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface,
                                               std::min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_fractionalScaleManagerImpl, data, nullptr);
}

// ---- xdg-shell ------------------------------------------------------------

static void positionerDestroyed(wl_resource* resource) {
    delete static_cast<Positioner*>(wl_resource_get_user_data(resource));
}
static void positionerDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void positionerSetSize(wl_client*, wl_resource* resource, int32_t width, int32_t height) {
    if (auto* p = static_cast<Positioner*>(wl_resource_get_user_data(resource))) {
        p->width = width; p->height = height;
    }
}
static void positionerSetAnchorRect(wl_client*, wl_resource* resource, int32_t x, int32_t y,
                                    int32_t width, int32_t height) {
    if (auto* p = static_cast<Positioner*>(wl_resource_get_user_data(resource))) {
        p->anchorX = x; p->anchorY = y; p->anchorWidth = width; p->anchorHeight = height;
    }
}
static void positionerSetAnchor(wl_client*, wl_resource* resource, uint32_t anchor) {
    if (auto* p = static_cast<Positioner*>(wl_resource_get_user_data(resource))) p->anchor = anchor;
}
static void positionerSetGravity(wl_client*, wl_resource* resource, uint32_t gravity) {
    if (auto* p = static_cast<Positioner*>(wl_resource_get_user_data(resource))) p->gravity = gravity;
}
static void positionerSetConstraint(wl_client*, wl_resource*, uint32_t) {}
static void positionerSetOffset(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    if (auto* p = static_cast<Positioner*>(wl_resource_get_user_data(resource))) {
        p->offsetX = x; p->offsetY = y;
    }
}
static void positionerSetReactive(wl_client*, wl_resource*) {}
static void positionerSetParentSize(wl_client*, wl_resource*, int32_t, int32_t) {}
static void positionerSetParentConfigure(wl_client*, wl_resource*, uint32_t) {}

static void xdgSurfaceResourceDestroyed(wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) surface->xdgSurface = nullptr;
}
static void xdgSurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void toplevelResourceDestroyed(wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) surface->xdgToplevel = nullptr;
}
static void toplevelDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void toplevelSetParent(wl_client*, wl_resource*, wl_resource*) {}
static void toplevelSetTitle(wl_client*, wl_resource* resource, const char* title) {
    if (auto* surface = surfaceFromResource(resource)) {
        surface->title = title ? title : "";
        if (!surface->title.empty()) surface->view->title = surface->title;
    }
}
static void toplevelSetAppId(wl_client*, wl_resource*, const char*) {}
static void toplevelShowWindowMenu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void toplevelMove(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void toplevelResize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void toplevelSetMaxSize(wl_client*, wl_resource*, int32_t, int32_t) {}
static void toplevelSetMinSize(wl_client*, wl_resource*, int32_t, int32_t) {}
static void toplevelSetMaximized(wl_client*, wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) sendToplevelConfigure(surface);
}
static void toplevelUnsetMaximized(wl_client*, wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) sendToplevelConfigure(surface);
}
static void toplevelSetFullscreen(wl_client*, wl_resource* resource, wl_resource*) {
    if (auto* surface = surfaceFromResource(resource)) {
        surface->fullscreenRequested = true;
        sendToplevelConfigure(surface);
    }
}
static void toplevelUnsetFullscreen(wl_client*, wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) {
        surface->fullscreenRequested = false;
        sendToplevelConfigure(surface);
    }
}
static void toplevelSetMinimized(wl_client*, wl_resource*) {}

static void xdgSurfaceGetToplevel(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    surface->xdgToplevel = wl_resource_create(client, &xdg_toplevel_interface,
                                              wl_resource_get_version(resource), id);
    if (!surface->xdgToplevel) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->xdgToplevel, &g_toplevelImpl, surface,
                                   toplevelResourceDestroyed);
    sendToplevelConfigure(surface);
}

static void popupResourceDestroyed(wl_resource* resource) {
    if (auto* surface = surfaceFromResource(resource)) surface->xdgPopup = nullptr;
}
static void popupDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void popupGrab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void popupReposition(wl_client*, wl_resource* resource, wl_resource* positionerResource,
                            uint32_t token) {
    auto* surface = surfaceFromResource(resource);
    auto* positioner = static_cast<Positioner*>(wl_resource_get_user_data(positionerResource));
    if (!surface) return;
    xdg_popup_send_repositioned(resource, token);
    sendPopupConfigure(surface, positioner);
}

static void xdgSurfaceGetPopup(wl_client* client, wl_resource* resource, uint32_t id,
                               wl_resource* parentResource, wl_resource* positionerResource) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    surface->parent = parentResource ? surfaceFromResource(parentResource) : rootSurface(surface->view);
    surface->xdgPopup = wl_resource_create(client, &xdg_popup_interface,
                                           wl_resource_get_version(resource), id);
    if (!surface->xdgPopup) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->xdgPopup, &g_popupImpl, surface, popupResourceDestroyed);
    auto* positioner = static_cast<Positioner*>(wl_resource_get_user_data(positionerResource));
    sendPopupConfigure(surface, positioner);
}

static void xdgSurfaceSetWindowGeometry(wl_client*, wl_resource* resource,
                                        int32_t x, int32_t y, int32_t width, int32_t height) {
    auto* surface = surfaceFromResource(resource);
    if (!surface) return;
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                               "window geometry must have positive width and height");
        return;
    }
    surface->pendingWindowGeometry = {
        .set = true,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
}
static void xdgSurfaceAckConfigure(wl_client*, wl_resource*, uint32_t) {}

static void xdgWmBaseDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void xdgWmBaseCreatePositioner(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* positioner = new Positioner();
    wl_resource* result = wl_resource_create(client, &xdg_positioner_interface,
                                             wl_resource_get_version(resource), id);
    if (!result) {
        delete positioner;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(result, &g_positionerImpl, positioner, positionerDestroyed);
}

static void xdgWmBaseGetSurface(wl_client* client, wl_resource* resource, uint32_t id,
                                wl_resource* surfaceResource) {
    auto* surface = surfaceFromResource(surfaceResource);
    if (!surface) return;
    surface->xdgSurface = wl_resource_create(client, &xdg_surface_interface,
                                             wl_resource_get_version(resource), id);
    if (!surface->xdgSurface) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->xdgSurface, &g_xdgSurfaceImpl, surface,
                                   xdgSurfaceResourceDestroyed);
}

static void xdgWmBasePong(wl_client*, wl_resource*, uint32_t) {}

static void bindXdgWmBase(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface, std::min(version, 3u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_xdgWmBaseImpl, data, nullptr);
}

// ---- wl_output ------------------------------------------------------------

static void outputResourceDestroyed(wl_resource* resource) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    if (!view) return;
    view->outputs.erase(std::remove_if(view->outputs.begin(), view->outputs.end(),
                                      [resource](const auto& item) { return item.resource == resource; }),
                        view->outputs.end());
}
static void outputRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void bindOutput(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* view = static_cast<AppView*>(data);
    const uint32_t v = std::min(version, 4u);
    wl_resource* resource = wl_resource_create(client, &wl_output_interface, v, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_outputImpl, view, outputResourceDestroyed);
    view->outputs.push_back({client, resource});
    wl_output_send_geometry(resource, 0, 0, 340, 190, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "wayland-appview", "embedded-output", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        view->hostWidth, view->hostHeight, 240000);
    if (v >= 2) {
        wl_output_send_scale(resource, std::max(1, static_cast<int>(std::lround(view->density))));
        wl_output_send_done(resource);
    }
    if (v >= 4) {
        wl_output_send_name(resource, "APPVIEW-1");
        wl_output_send_description(resource, "Wayland AppView embedded output");
        wl_output_send_done(resource);
    }
}

// ---- wl_seat / input ------------------------------------------------------

static void pointerResourceDestroyed(wl_resource* resource) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    if (!view) return;
    for (auto& seat : view->seats) if (seat.pointer == resource) seat.pointer = nullptr;
}
static void keyboardResourceDestroyed(wl_resource* resource) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    if (!view) return;
    for (auto& seat : view->seats) if (seat.keyboard == resource) seat.keyboard = nullptr;
}
static void seatResourceDestroyed(wl_resource* resource) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    if (!view) return;
    view->seats.erase(std::remove_if(view->seats.begin(), view->seats.end(),
                                    [resource](const auto& item) { return item.seat == resource; }),
                      view->seats.end());
}

static void pointerSetCursor(wl_client*, wl_resource* resource, uint32_t,
                             wl_resource* surfaceResource, int32_t hotspotX, int32_t hotspotY) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    if (!view) return;
    if (!surfaceResource) {
        view->cursorSurface = nullptr;
        if (view->customCursor) {
            SDL_DestroyCursor(view->customCursor);
            view->customCursor = nullptr;
        }
        SDL_HideCursor();
        return;
    }

    auto* surface = surfaceFromResource(surfaceResource);
    if (!surface) return;
    surface->cursorRole = true;
    view->cursorSurface = surface;
    view->cursorHotspotX = hotspotX;
    view->cursorHotspotY = hotspotY;
    applyHostCursor(view);
}
static void pointerRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void keyboardRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void seatGetPointer(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    auto* binding = seatBindingForSeatResource(view, resource);
    if (!binding) return;
    binding->pointer = wl_resource_create(client, &wl_pointer_interface,
                                          std::min(wl_resource_get_version(resource), 7), id);
    if (!binding->pointer) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(binding->pointer, &g_pointerImpl, view, pointerResourceDestroyed);
}

static void seatGetKeyboard(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    auto* binding = seatBindingForSeatResource(view, resource);
    if (!binding) return;
    binding->keyboard = wl_resource_create(client, &wl_keyboard_interface,
                                           std::min(wl_resource_get_version(resource), 7), id);
    if (!binding->keyboard) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(binding->keyboard, &g_keyboardImpl, view, keyboardResourceDestroyed);
    size_t size = 0;
    int fd = createKeymapFd(view, &size);
    if (fd >= 0) {
        wl_keyboard_send_keymap(binding->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                static_cast<uint32_t>(size));
        close(fd);
    }
    if (wl_resource_get_version(binding->keyboard) >= 4) wl_keyboard_send_repeat_info(binding->keyboard, 25, 600);
}

static void seatGetTouch(wl_client*, wl_resource*, uint32_t) {}
static void seatRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void bindSeat(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* view = static_cast<AppView*>(data);
    const uint32_t v = std::min(version, 7u);
    wl_resource* resource = wl_resource_create(client, &wl_seat_interface, v, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    view->seats.push_back({client, resource, nullptr, nullptr});
    wl_resource_set_implementation(resource, &g_seatImpl, view, seatResourceDestroyed);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= 2) wl_seat_send_name(resource, "appview-seat");
}

// ---- wl_data_device_manager ----------------------------------------------

static void dataSourceOffer(wl_client*, wl_resource*, const char*) {}
static void dataSourceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void dataSourceSetActions(wl_client*, wl_resource*, uint32_t) {}

static void dataDeviceStartDrag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                                wl_resource*, uint32_t) {}
static void dataDeviceSetSelection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void dataDeviceRelease(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static void dataDeviceManagerCreateSource(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* source = wl_resource_create(
        client, &wl_data_source_interface, std::min(wl_resource_get_version(resource), 3), id);
    if (!source) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(source, &g_dataSourceImpl, nullptr, nullptr);
}

static void dataDeviceManagerGetDevice(wl_client* client, wl_resource* resource, uint32_t id,
                                       wl_resource*) {
    wl_resource* device = wl_resource_create(
        client, &wl_data_device_interface, std::min(wl_resource_get_version(resource), 3), id);
    if (!device) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &g_dataDeviceImpl, nullptr, nullptr);
}

static void dataDeviceManagerRelease(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void bindDataDeviceManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &wl_data_device_manager_interface, std::min(version, 3u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_dataDeviceManagerImpl, data, nullptr);
}

// ---- linux-dmabuf ---------------------------------------------------------

static void dmaBufferResourceDestroyed(wl_resource* resource) {
    g_dmaBuffers.erase(resource);
    delete static_cast<DmaBuffer*>(wl_resource_get_user_data(resource));
}
static void dmaBufferDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static bool validateParams(DmaParams* params, int width, int height) {
    return params && !params->used && width > 0 && height > 0 && params->planes[0].set;
}

static wl_resource* createDmaBufferResource(wl_client* client, uint32_t id, DmaParams* params,
                                            int width, int height, uint32_t format, uint32_t flags) {
    if (!validateParams(params, width, height)) return nullptr;
    auto* buffer = new DmaBuffer();
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->flags = flags;
    for (size_t i = 0; i < params->planes.size(); ++i) {
        if (!params->planes[i].set) continue;
        buffer->planes[i] = params->planes[i];
        params->planes[i].fd = -1;
        params->planes[i].set = false;
    }
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        delete buffer;
        return nullptr;
    }
    wl_resource_set_implementation(buffer->resource, &g_dmaBufferImpl, buffer, dmaBufferResourceDestroyed);
    g_dmaBuffers[buffer->resource] = buffer;
    params->used = true;
    return buffer->resource;
}

static void dmaParamsResourceDestroyed(wl_resource* resource) {
    delete static_cast<DmaParams*>(wl_resource_get_user_data(resource));
}
static void dmaParamsDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void dmaParamsAdd(wl_client*, wl_resource* resource, int32_t fd, uint32_t planeIndex,
                         uint32_t offset, uint32_t stride, uint32_t modifierHi, uint32_t modifierLo) {
    auto* params = static_cast<DmaParams*>(wl_resource_get_user_data(resource));
    if (!params || planeIndex >= params->planes.size() || params->planes[planeIndex].set) {
        if (fd >= 0) close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "invalid DMA-BUF plane index");
        return;
    }
    auto& plane = params->planes[planeIndex];
    plane.fd = fd;
    plane.offset = offset;
    plane.stride = stride;
    plane.modifier = (static_cast<uint64_t>(modifierHi) << 32) | modifierLo;
    plane.set = true;
}

static void dmaParamsCreate(wl_client* client, wl_resource* resource, int32_t width, int32_t height,
                            uint32_t format, uint32_t flags) {
    auto* params = static_cast<DmaParams*>(wl_resource_get_user_data(resource));
    wl_resource* buffer = createDmaBufferResource(client, 0, params, width, height, format, flags);
    if (!buffer) {
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    zwp_linux_buffer_params_v1_send_created(resource, buffer);
}

static void dmaParamsCreateImmediate(wl_client* client, wl_resource* resource, uint32_t id,
                                     int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    auto* params = static_cast<DmaParams*>(wl_resource_get_user_data(resource));
    if (!createDmaBufferResource(client, id, params, width, height, format, flags)) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                               "failed to create DMA-BUF wl_buffer");
    }
}

static void dmabufDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void dmabufCreateParams(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* view = static_cast<AppView*>(wl_resource_get_user_data(resource));
    auto* params = new DmaParams();
    params->view = view;
    params->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                          wl_resource_get_version(resource), id);
    if (!params->resource) {
        delete params;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(params->resource, &g_dmaParamsImpl, params, dmaParamsResourceDestroyed);
}
static void dmabufGetDefaultFeedback(wl_client*, wl_resource*, uint32_t) {}
static void dmabufGetSurfaceFeedback(wl_client*, wl_resource*, uint32_t, wl_resource*) {}

static void bindDmabuf(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* view = static_cast<AppView*>(data);
    const uint32_t v = std::min(version, 3u);
    wl_resource* resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, v, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &g_dmabufImpl, view, nullptr);
    for (const auto& [format, modifier] : view->dmaFormats) {
        if (v >= 3 && modifier != DRM_FORMAT_MOD_INVALID) {
            zwp_linux_dmabuf_v1_send_modifier(resource, format,
                                               static_cast<uint32_t>(modifier >> 32),
                                               static_cast<uint32_t>(modifier & 0xffffffffu));
        } else {
            zwp_linux_dmabuf_v1_send_format(resource, format);
        }
    }
}

static void initializeInterfaces() {
    if (g_interfacesInitialized) return;
    g_interfacesInitialized = true;

    g_compositorImpl.create_surface = compositorCreateSurface;
    g_compositorImpl.create_region = compositorCreateRegion;

    g_regionImpl.destroy = regionDestroy;
    g_regionImpl.add = regionAdd;
    g_regionImpl.subtract = regionSubtract;

    g_surfaceImpl.destroy = surfaceDestroy;
    g_surfaceImpl.attach = surfaceAttach;
    g_surfaceImpl.damage = surfaceDamage;
    g_surfaceImpl.frame = surfaceFrame;
    g_surfaceImpl.set_opaque_region = surfaceSetOpaque;
    g_surfaceImpl.set_input_region = surfaceSetInput;
    g_surfaceImpl.commit = surfaceCommit;
    g_surfaceImpl.set_buffer_transform = surfaceSetBufferTransform;
    g_surfaceImpl.set_buffer_scale = surfaceSetBufferScale;
    g_surfaceImpl.damage_buffer = surfaceDamageBuffer;
    g_surfaceImpl.offset = surfaceOffset;

    g_subcompositorImpl.destroy = subcompositorDestroy;
    g_subcompositorImpl.get_subsurface = subcompositorGetSubsurface;
    g_subsurfaceImpl.destroy = subsurfaceDestroy;
    g_subsurfaceImpl.set_position = subsurfaceSetPosition;
    g_subsurfaceImpl.place_above = subsurfacePlaceAbove;
    g_subsurfaceImpl.place_below = subsurfacePlaceBelow;
    g_subsurfaceImpl.set_sync = subsurfaceSetSync;
    g_subsurfaceImpl.set_desync = subsurfaceSetDesync;

    g_viewporterImpl.destroy = viewporterDestroy;
    g_viewporterImpl.get_viewport = viewporterGetViewport;
    g_viewportImpl.destroy = viewportDestroy;
    g_viewportImpl.set_source = viewportSetSource;
    g_viewportImpl.set_destination = viewportSetDestination;

    g_fractionalScaleManagerImpl.destroy = fractionalScaleManagerDestroy;
    g_fractionalScaleManagerImpl.get_fractional_scale = fractionalScaleManagerGet;
    g_fractionalScaleImpl.destroy = fractionalScaleDestroy;

    g_xdgWmBaseImpl.destroy = xdgWmBaseDestroy;
    g_xdgWmBaseImpl.create_positioner = xdgWmBaseCreatePositioner;
    g_xdgWmBaseImpl.get_xdg_surface = xdgWmBaseGetSurface;
    g_xdgWmBaseImpl.pong = xdgWmBasePong;

    g_positionerImpl.destroy = positionerDestroy;
    g_positionerImpl.set_size = positionerSetSize;
    g_positionerImpl.set_anchor_rect = positionerSetAnchorRect;
    g_positionerImpl.set_anchor = positionerSetAnchor;
    g_positionerImpl.set_gravity = positionerSetGravity;
    g_positionerImpl.set_constraint_adjustment = positionerSetConstraint;
    g_positionerImpl.set_offset = positionerSetOffset;
    g_positionerImpl.set_reactive = positionerSetReactive;
    g_positionerImpl.set_parent_size = positionerSetParentSize;
    g_positionerImpl.set_parent_configure = positionerSetParentConfigure;

    g_xdgSurfaceImpl.destroy = xdgSurfaceDestroy;
    g_xdgSurfaceImpl.get_toplevel = xdgSurfaceGetToplevel;
    g_xdgSurfaceImpl.get_popup = xdgSurfaceGetPopup;
    g_xdgSurfaceImpl.set_window_geometry = xdgSurfaceSetWindowGeometry;
    g_xdgSurfaceImpl.ack_configure = xdgSurfaceAckConfigure;

    g_toplevelImpl.destroy = toplevelDestroy;
    g_toplevelImpl.set_parent = toplevelSetParent;
    g_toplevelImpl.set_title = toplevelSetTitle;
    g_toplevelImpl.set_app_id = toplevelSetAppId;
    g_toplevelImpl.show_window_menu = toplevelShowWindowMenu;
    g_toplevelImpl.move = toplevelMove;
    g_toplevelImpl.resize = toplevelResize;
    g_toplevelImpl.set_max_size = toplevelSetMaxSize;
    g_toplevelImpl.set_min_size = toplevelSetMinSize;
    g_toplevelImpl.set_maximized = toplevelSetMaximized;
    g_toplevelImpl.unset_maximized = toplevelUnsetMaximized;
    g_toplevelImpl.set_fullscreen = toplevelSetFullscreen;
    g_toplevelImpl.unset_fullscreen = toplevelUnsetFullscreen;
    g_toplevelImpl.set_minimized = toplevelSetMinimized;

    g_popupImpl.destroy = popupDestroy;
    g_popupImpl.grab = popupGrab;
    g_popupImpl.reposition = popupReposition;

    g_outputImpl.release = outputRelease;

    g_seatImpl.get_pointer = seatGetPointer;
    g_seatImpl.get_keyboard = seatGetKeyboard;
    g_seatImpl.get_touch = seatGetTouch;
    g_seatImpl.release = seatRelease;
    g_pointerImpl.set_cursor = pointerSetCursor;
    g_pointerImpl.release = pointerRelease;
    g_keyboardImpl.release = keyboardRelease;

    g_dataDeviceManagerImpl.create_data_source = dataDeviceManagerCreateSource;
    g_dataDeviceManagerImpl.get_data_device = dataDeviceManagerGetDevice;
    g_dataSourceImpl.offer = dataSourceOffer;
    g_dataSourceImpl.destroy = dataSourceDestroy;
    g_dataSourceImpl.set_actions = dataSourceSetActions;
    g_dataDeviceImpl.start_drag = dataDeviceStartDrag;
    g_dataDeviceImpl.set_selection = dataDeviceSetSelection;
    g_dataDeviceImpl.release = dataDeviceRelease;

    g_dmaBufferImpl.destroy = dmaBufferDestroy;
    g_dmabufImpl.destroy = dmabufDestroy;
    g_dmabufImpl.create_params = dmabufCreateParams;
    g_dmabufImpl.get_default_feedback = dmabufGetDefaultFeedback;
    g_dmabufImpl.get_surface_feedback = dmabufGetSurfaceFeedback;
    g_dmaParamsImpl.destroy = dmaParamsDestroy;
    g_dmaParamsImpl.add = dmaParamsAdd;
    g_dmaParamsImpl.create = dmaParamsCreate;
    g_dmaParamsImpl.create_immed = dmaParamsCreateImmediate;
}

static bool initializeXkb(AppView* view) {
    view->xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!view->xkbContext) return false;
    view->xkbKeymap = xkb_keymap_new_from_names(view->xkbContext, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!view->xkbKeymap) return false;
    view->xkbState = xkb_state_new(view->xkbKeymap);
    return view->xkbState != nullptr;
}

static bool initializeWayland(AppView* view) {
    initializeInterfaces();
    view->display = wl_display_create();
    if (!view->display) {
        setError(view, "wl_display_create failed");
        return false;
    }
    view->loop = wl_display_get_event_loop(view->display);
    const char* socket = wl_display_add_socket_auto(view->display);
    if (!socket) {
        setError(view, std::string("wl_display_add_socket_auto failed: ") + std::strerror(errno));
        return false;
    }
    view->socketName = socket;
    if (wl_display_init_shm(view->display) != 0) {
        setError(view, "wl_display_init_shm failed");
        return false;
    }
    view->compositorGlobal = wl_global_create(view->display, &wl_compositor_interface, 5, view, bindCompositor);
    view->subcompositorGlobal = wl_global_create(view->display, &wl_subcompositor_interface, 1, view, bindSubcompositor);
    view->xdgGlobal = wl_global_create(view->display, &xdg_wm_base_interface, 3, view, bindXdgWmBase);
    view->outputGlobal = wl_global_create(view->display, &wl_output_interface, 4, view, bindOutput);
    view->seatGlobal = wl_global_create(view->display, &wl_seat_interface, 7, view, bindSeat);
    view->dataDeviceGlobal = wl_global_create(
        view->display, &wl_data_device_manager_interface, 3, view, bindDataDeviceManager);
    view->viewporterGlobal = wl_global_create(view->display, &wp_viewporter_interface, 1, view, bindViewporter);
    view->fractionalScaleGlobal = wl_global_create(
        view->display, &wp_fractional_scale_manager_v1_interface, 1, view, bindFractionalScaleManager);
    if (!view->compositorGlobal || !view->subcompositorGlobal || !view->xdgGlobal ||
        !view->outputGlobal || !view->seatGlobal || !view->dataDeviceGlobal ||
        !view->viewporterGlobal || !view->fractionalScaleGlobal) {
        setError(view, "failed to create one or more Wayland globals");
        return false;
    }
    if (!initializeXkb(view)) {
        setError(view, "failed to initialize xkbcommon keymap");
        return false;
    }
    return true;
}

static void ensureDmabufGlobal(AppView* view) {
    if (view->dmabufGlobal) return;
    view->dmabufGlobal = wl_global_create(view->display, &zwp_linux_dmabuf_v1_interface, 3, view, bindDmabuf);
    if (!view->dmabufGlobal) setError(view, "failed to create linux-dmabuf global");
}

static void reapChild(AppView* view) {
    if (!view || view->childPid <= 0) return;
    int status = 0;
    pid_t result = waitpid(view->childPid, &status, WNOHANG);
    if (result == view->childPid) view->childPid = -1;
}

} // namespace

struct appview : AppView {};

extern "C" appview_t* appview_create(void) {
    auto* view = new appview();
    if (!initializeWayland(view)) {
        // Keep the object alive so Kotlin can surface appview_error().
    }
    return view;
}

extern "C" void appview_destroy(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return;
    appview_terminate(view);
    if (view->cursorSurface || view->customCursor) resetHostCursor(view);
    if (view->renderer.initialized) {
        glDeleteBuffers(1, &view->renderer.vbo);
        glDeleteVertexArrays(1, &view->renderer.vao);
        glDeleteProgram(view->renderer.program);
    }
    if (view->xkbState) xkb_state_unref(view->xkbState);
    if (view->xkbKeymap) xkb_keymap_unref(view->xkbKeymap);
    if (view->xkbContext) xkb_context_unref(view->xkbContext);
    if (view->display) wl_display_destroy(view->display);
    delete view;
}

extern "C" const char* appview_socket_name(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    return view ? view->socketName.c_str() : "";
}

extern "C" const char* appview_error(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    return view ? view->error.c_str() : "invalid appview";
}

extern "C" const char* appview_title(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    return view ? view->title.c_str() : "";
}

extern "C" int appview_child_pid(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return -1;
    reapChild(view);
    if (view->childPid > 0) return static_cast<int>(view->childPid);
    return view->pendingCommand.empty() ? -1 : 0;
}

extern "C" int appview_is_mapped(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return 0;
    return rootSurface(view) ? 1 : 0;
}

extern "C" int appview_fullscreen_requested(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return 0;
    for (auto& surface : view->surfaces) {
        if (surface->xdgToplevel && surface->fullscreenRequested) return 1;
    }
    return 0;
}

extern "C" int appview_launch(appview_t* opaque, const char* command) {
    auto* view = static_cast<appview*>(opaque);
    if (!view || !view->display || !command || !*command) return 0;
    appview_terminate(view);
    view->pendingCommand = command;
    view->error.clear();
    view->sceneDirty = true;
    return 1;
}

extern "C" void appview_terminate(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return;
    view->pendingCommand.clear();
    for (auto& surface : view->surfaces) surface->fullscreenRequested = false;
    if (view->cursorSurface || view->customCursor) resetHostCursor(view);
    if (view->childPid > 0) {
        kill(view->childPid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            pid_t result = waitpid(view->childPid, &status, WNOHANG);
            if (result == view->childPid) {
                view->childPid = -1;
                break;
            }
            usleep(10000);
        }
        if (view->childPid > 0) {
            kill(view->childPid, SIGKILL);
            waitpid(view->childPid, nullptr, 0);
            view->childPid = -1;
        }
    }
}

extern "C" int appview_render(appview_t* opaque, int framebuffer, int width, int height, float density) {
    auto* view = static_cast<appview*>(opaque);
    if (!view || !view->display) return 0;
    const float newDensity = std::max(0.1f, density);
    const bool densityChanged = std::fabs(view->density - newDensity) > 0.001f;
    const bool sizeChanged = view->hostWidth != width || view->hostHeight != height || densityChanged;
    view->hostWidth = std::max(1, width);
    view->hostHeight = std::max(1, height);
    view->density = newDensity;
    if (!hostPointerInsideAppView(view) &&
        (view->pointerFocus || view->cursorSurface || view->customCursor)) {
        leaveAppViewPointer(view);
    }
    if (!initializeEgl(view)) return 0;
    ensureDmabufGlobal(view);
    if (!view->dmabufGlobal) return 0;
    if (!spawnPendingChild(view)) return 0;
    wl_event_loop_dispatch(view->loop, 0);
    wl_display_flush_clients(view->display);
    reapChild(view);
    if (sizeChanged) {
        view->sceneDirty = true;
        for (auto& surface : view->surfaces) {
            if (surface->xdgToplevel) sendToplevelConfigure(surface.get());
            if (densityChanged && surface->fractionalScale) {
                wp_fractional_scale_v1_send_preferred_scale(
                    surface->fractionalScale, preferredFractionalScale(view));
            }
        }
    }
    return renderScene(view, framebuffer) ? 1 : 0;
}

extern "C" void appview_pointer_motion(appview_t* opaque, int x, int y, unsigned int timeMs) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return;

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_GetMouseState(&mouseX, &mouseY);
    const double mousePixelX = static_cast<double>(mouseX) * view->density;
    const double mousePixelY = static_cast<double>(mouseY) * view->density;

    // NativeView currently folds Compose Enter/Exit events into an ordinary
    // InteropPointerEventType.Move. Establish the view's window-space origin
    // while the host pointer is known to be inside, then use SDL's current
    // pointer position to detect a real exit even if Compose reports the last
    // in-bounds local position for its Exit event.
    if (!view->pointerWindowOriginKnown) {
        view->pointerWindowOriginX = mousePixelX - x;
        view->pointerWindowOriginY = mousePixelY - y;
        view->pointerWindowOriginKnown = true;
    } else if (hostPointerInsideAppView(view)) {
        view->pointerWindowOriginX = mousePixelX - x;
        view->pointerWindowOriginY = mousePixelY - y;
    } else {
        leaveAppViewPointer(view);
        return;
    }

    view->pointerX = x / view->density;
    view->pointerY = y / view->density;
    Surface* target = hitTest(view, view->pointerX, view->pointerY);
    updatePointerFocus(view, target, timeMs);
    if (!target || !target->resource) return;
    wl_client* client = wl_resource_get_client(target->resource);
    auto [sx, sy] = absolutePosition(target);
    for (auto& seat : view->seats) {
        if (seat.client != client || !seat.pointer) continue;
        wl_pointer_send_motion(seat.pointer, timeMs ? timeMs : nowMs(),
                               wl_fixed_from_double(view->pointerX - sx),
                               wl_fixed_from_double(view->pointerY - sy));
        if (wl_resource_get_version(seat.pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(seat.pointer);
    }
    wl_display_flush_clients(view->display);
}

extern "C" void appview_pointer_leave(appview_t* opaque) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return;
    leaveAppViewPointer(view);
}

extern "C" void appview_pointer_button(appview_t* opaque, int button, int pressed,
                                        unsigned int timeMs) {
    auto* view = static_cast<appview*>(opaque);
    if (!view || !view->pointerFocus || !view->pointerFocus->resource) return;
    wl_client* client = wl_resource_get_client(view->pointerFocus->resource);
    uint32_t linuxButton = BTN_LEFT;
    if (button == 2) linuxButton = BTN_MIDDLE;
    else if (button == 3) linuxButton = BTN_RIGHT;
    for (auto& seat : view->seats) {
        if (seat.client != client || !seat.pointer) continue;
        wl_pointer_send_button(seat.pointer, nextSerial(view), timeMs ? timeMs : nowMs(), linuxButton,
                               pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        if (wl_resource_get_version(seat.pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(seat.pointer);
    }
    if (pressed) updateKeyboardFocus(view, view->pointerFocus);
    wl_display_flush_clients(view->display);
}

extern "C" void appview_scroll(appview_t* opaque, double dx, double dy, unsigned int timeMs) {
    auto* view = static_cast<appview*>(opaque);
    if (!view || !view->pointerFocus || !view->pointerFocus->resource) return;
    wl_client* client = wl_resource_get_client(view->pointerFocus->resource);
    const uint32_t time = timeMs ? timeMs : nowMs();
    for (auto& seat : view->seats) {
        if (seat.client != client || !seat.pointer) continue;
        const uint32_t version = wl_resource_get_version(seat.pointer);
        if (version >= 5) wl_pointer_send_axis_source(seat.pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
        if (std::fabs(dx) > 0.001) wl_pointer_send_axis(seat.pointer, time,
                                                       WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                                       wl_fixed_from_double(dx * 12.0));
        if (std::fabs(dy) > 0.001) wl_pointer_send_axis(seat.pointer, time,
                                                       WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                       wl_fixed_from_double(dy * 12.0));
        if (version >= 5) wl_pointer_send_frame(seat.pointer);
    }
    wl_display_flush_clients(view->display);
}

extern "C" void appview_key(appview_t* opaque, unsigned int evdevKey, int pressed,
                             unsigned int timeMs) {
    auto* view = static_cast<appview*>(opaque);
    if (!view || !view->hostFocused) return;
    Surface* target = view->keyboardFocus ? view->keyboardFocus : rootSurface(view);
    if (!target || !target->resource) return;
    wl_client* client = wl_resource_get_client(target->resource);
    if (view->keyboardFocus != target) updateKeyboardFocus(view, target);
    if (view->xkbState) {
        xkb_state_update_key(view->xkbState, evdevKey + 8,
                             pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    }
    for (auto& seat : view->seats) {
        if (seat.client != client || !seat.keyboard) continue;
        wl_keyboard_send_key(seat.keyboard, nextSerial(view), timeMs ? timeMs : nowMs(), evdevKey,
                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        sendKeyboardModifiers(view, seat.keyboard);
    }
    wl_display_flush_clients(view->display);
}

extern "C" void appview_set_focused(appview_t* opaque, int focused) {
    auto* view = static_cast<appview*>(opaque);
    if (!view) return;
    const bool nextFocused = focused != 0;
    const bool changed = view->hostFocused != nextFocused;
    view->hostFocused = nextFocused;
    updateKeyboardFocus(view, view->hostFocused ? rootSurface(view) : nullptr);
    if (changed) {
        for (auto& surface : view->surfaces) {
            if (surface->xdgToplevel) sendToplevelConfigure(surface.get());
        }
    }
    wl_display_flush_clients(view->display);
}
