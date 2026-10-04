#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct appview appview_t;

appview_t* appview_create(void);
void appview_destroy(appview_t* view);

const char* appview_socket_name(appview_t* view);
const char* appview_error(appview_t* view);
const char* appview_title(appview_t* view);
int appview_child_pid(appview_t* view);
int appview_is_mapped(appview_t* view);
int appview_fullscreen_requested(appview_t* view);
int appview_text_input_active(appview_t* view);

int appview_launch(appview_t* view, const char* command);
void appview_terminate(appview_t* view);

int appview_render(appview_t* view, int framebuffer, int width, int height, float density);
void appview_pointer_motion(appview_t* view, int x, int y, unsigned int time_ms);
void appview_pointer_leave(appview_t* view);
void appview_pointer_button(appview_t* view, int button, int pressed, unsigned int time_ms);
void appview_scroll(appview_t* view, double dx, double dy, unsigned int time_ms);
void appview_key(appview_t* view, unsigned int evdev_key, int pressed, unsigned int time_ms);
void appview_text_input_preedit(appview_t* view, const char* text, int cursor_begin, int cursor_end);
void appview_text_input_commit(appview_t* view, const char* text);
void appview_text_input_delete_surrounding(appview_t* view, unsigned int before_length, unsigned int after_length);
void appview_set_focused(appview_t* view, int focused);

#ifdef __cplusplus
}
#endif
