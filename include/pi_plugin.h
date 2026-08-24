#ifndef PI_PLUGIN_H
#define PI_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

/*
 * pi-cplugins C ABI
 *
 * The ABI has one registration entrypoint. Public structures grow through
 * reserved pointer slots: providers set unused slots to NULL and consumers
 * ignore slots they do not understand.
 */

#define PI_PLUGIN_MAX_CALL_BYTES 128u
#define PI_PLUGIN_MAX_JSON_BYTES (64u * 1024u)
#define PI_PLUGIN_RESERVED_SLOTS 8u

#define PI_PLUGIN_OK 0
#define PI_PLUGIN_ERR_INVALID_ARGUMENT 1
#define PI_PLUGIN_ERR_BUFFER_TOO_SMALL 2
#define PI_PLUGIN_ERR_PAYLOAD_TOO_LARGE 3
#define PI_PLUGIN_ERR_OOM 4

#define PI_PLUGIN_UI_INFO 0
#define PI_PLUGIN_UI_WARNING 1
#define PI_PLUGIN_UI_ERROR 2

#define PI_PLUGIN_WIDGET_ABOVE_EDITOR 0
#define PI_PLUGIN_WIDGET_BELOW_EDITOR 1

typedef void* (*pi_plugin_host_malloc_fn)(void* host_data, size_t size);
typedef void (*pi_plugin_host_free_fn)(void* host_data, void* ptr);

typedef struct pi_plugin_callback_context {
  void* host_call_data;
  const char* cwd;
  size_t cwd_size;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
} pi_plugin_callback_context;

typedef int32_t (*pi_plugin_callback_fn)(
    void* callback_data, const pi_plugin_callback_context* context,
    const char* input_json, size_t input_size, const char** output_json,
    size_t* output_size);

typedef struct pi_plugin_tool {
  const char* name;
  const char* label;
  const char* description;
  const char* parameters_json;
  pi_plugin_callback_fn execute;
  void* callback_data;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
} pi_plugin_tool;

typedef struct pi_plugin_command {
  const char* name;
  const char* description;
  pi_plugin_callback_fn execute;
  void* callback_data;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
} pi_plugin_command;

typedef struct pi_plugin_event_handler {
  const char* event_name;
  pi_plugin_callback_fn execute;
  void* callback_data;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
} pi_plugin_event_handler;

typedef int32_t (*pi_plugin_register_tool_fn)(void* host_data,
                                               const pi_plugin_tool* tool);
typedef int32_t (*pi_plugin_register_command_fn)(
    void* host_data, const pi_plugin_command* command);
typedef int32_t (*pi_plugin_register_event_handler_fn)(
    void* host_data, const pi_plugin_event_handler* handler);
typedef int32_t (*pi_plugin_is_cancelled_fn)(void* host_data,
                                              void* host_call_data);
typedef int32_t (*pi_plugin_tool_update_fn)(void* host_data,
                                             void* host_call_data,
                                             const char* update_json,
                                             size_t update_size);
typedef int32_t (*pi_plugin_ui_notify_fn)(void* host_data,
                                           void* host_call_data,
                                           const char* message,
                                           size_t message_size, int32_t level);
typedef int32_t (*pi_plugin_ui_set_status_fn)(
    void* host_data, void* host_call_data, const char* key, size_t key_size,
    const char* text, size_t text_size);
typedef int32_t (*pi_plugin_ui_set_widget_fn)(
    void* host_data, void* host_call_data, const char* key, size_t key_size,
    const char* lines_json, size_t lines_size, int32_t placement);

typedef struct pi_plugin_host_api {
  void* host_data;
  pi_plugin_host_malloc_fn malloc;
  pi_plugin_host_free_fn free;
  pi_plugin_register_tool_fn register_tool;
  pi_plugin_register_command_fn register_command;
  pi_plugin_register_event_handler_fn on;
  pi_plugin_is_cancelled_fn is_cancelled;
  pi_plugin_tool_update_fn tool_update;
  pi_plugin_ui_notify_fn ui_notify;
  pi_plugin_ui_set_status_fn ui_set_status;
  pi_plugin_ui_set_widget_fn ui_set_widget;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
} pi_plugin_host_api;

static inline void pi_plugin_copy_host_api(pi_plugin_host_api* destination,
                                           const pi_plugin_host_api* source) {
  destination->host_data = source->host_data;
  destination->malloc = source->malloc;
  destination->free = source->free;
  destination->register_tool = source->register_tool;
  destination->register_command = source->register_command;
  destination->on = source->on;
  destination->is_cancelled = source->is_cancelled;
  destination->tool_update = source->tool_update;
  destination->ui_notify = source->ui_notify;
  destination->ui_set_status = source->ui_set_status;
  destination->ui_set_widget = source->ui_set_widget;
  for (size_t i = 0u; i < PI_PLUGIN_RESERVED_SLOTS; ++i) {
    destination->reserved[i] = NULL;
  }
}

typedef struct pi_plugin_api pi_plugin_api;
typedef const char* (*pi_plugin_manifest_fn)(const pi_plugin_api* api,
                                             size_t* byte_count);
typedef int32_t (*pi_plugin_call_fn)(pi_plugin_api* api, const uint8_t* input,
                                    size_t input_size, uint8_t* output,
                                    size_t output_capacity,
                                    size_t* output_size);
typedef int32_t (*pi_plugin_close_fn)(pi_plugin_api* api);

struct pi_plugin_api {
  size_t max_payload_bytes;
  pi_plugin_manifest_fn manifest;
  pi_plugin_call_fn call;
  pi_plugin_close_fn close;
  void* reserved[PI_PLUGIN_RESERVED_SLOTS];
};

typedef pi_plugin_api* (*pi_plugin_init_fn)(const pi_plugin_host_api* host);

/*
 * Extensions copy named host fields with pi_plugin_copy_host_api(); copying the
 * whole structure would read a reserved tail that may be shorter on an older
 * host. Registration descriptors and their strings remain valid until close.
 * Callback input and context are borrowed for the callback duration. Callback
 * output is borrowed by the host and copied before the callback returns to Pi.
 * UI and update functions may be called only while a callback is active.
 * close invalidates the plugin API, descriptors, callbacks, and relocated code.
 */
pi_plugin_api* pi_plugin_init(const pi_plugin_host_api* host);

#endif /* PI_PLUGIN_H */
