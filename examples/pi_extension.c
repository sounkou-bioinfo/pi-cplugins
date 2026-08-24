#include "pi_plugin.h"

#include <string.h>

typedef struct extension_state {
  pi_plugin_api api;
  pi_plugin_host_api host;
  int closed;
} extension_state;

static extension_state state;

static int32_t demo_tool(void* callback_data,
                         const pi_plugin_callback_context* context,
                         const char* input_json, size_t input_size,
                         const char** output_json, size_t* output_size) {
  extension_state* extension = (extension_state*)callback_data;
  (void)input_size;
  if (extension == NULL || context == NULL || input_json == NULL ||
      output_json == NULL || output_size == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (extension->host.is_cancelled(
          extension->host.host_data, context->host_call_data)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  static const char update[] =
      "{\"content\":[{\"type\":\"text\",\"text\":\"C tool is running\"}]}";
  extension->host.tool_update(extension->host.host_data,
                              context->host_call_data, update,
                              sizeof(update) - 1u);
  static const char notice[] = "C registered and executed this Pi tool";
  extension->host.ui_notify(extension->host.host_data,
                            context->host_call_data, notice,
                            sizeof(notice) - 1u, PI_PLUGIN_UI_INFO);
  extension->host.ui_set_status(extension->host.host_data,
                                context->host_call_data,
                                "c-extension", 11u, "active", 6u);
  static const char widget[] =
      "[\"pi_plugin_init registered this tool\",\"TinyCC owns its callback code\"]";
  extension->host.ui_set_widget(extension->host.host_data,
                                context->host_call_data,
                                "c-extension", 11u, widget,
                                sizeof(widget) - 1u,
                                PI_PLUGIN_WIDGET_ABOVE_EDITOR);

  static const char result[] =
      "{\"content\":[{\"type\":\"text\",\"text\":\"Hello from a C-authored Pi extension\"}],"
      "\"details\":{\"implementation\":\"examples/pi_extension.c\"}}";
  *output_json = result;
  *output_size = sizeof(result) - 1u;
  return PI_PLUGIN_OK;
}

static int32_t demo_command(void* callback_data,
                            const pi_plugin_callback_context* context,
                            const char* input_json, size_t input_size,
                            const char** output_json, size_t* output_size) {
  (void)input_json;
  (void)input_size;
  extension_state* extension = (extension_state*)callback_data;
  if (extension == NULL || context == NULL || output_json == NULL ||
      output_size == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  static const char notice[] =
      "The /c-extension-demo command is implemented in C";
  extension->host.ui_notify(extension->host.host_data,
                            context->host_call_data, notice,
                            sizeof(notice) - 1u, PI_PLUGIN_UI_INFO);
  *output_json = NULL;
  *output_size = 0u;
  return PI_PLUGIN_OK;
}

static int32_t before_agent(void* callback_data,
                            const pi_plugin_callback_context* context,
                            const char* input_json, size_t input_size,
                            const char** output_json, size_t* output_size) {
  (void)context;
  (void)input_json;
  (void)input_size;
  extension_state* extension = (extension_state*)callback_data;
  static const char transform[] =
      "{\"systemPrompt\":\"This system prompt was returned directly by a TinyCC C callback.\"}";
  if (extension == NULL || output_json == NULL || output_size == NULL ||
      extension->closed) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  *output_json = transform;
  *output_size = sizeof(transform) - 1u;
  return PI_PLUGIN_OK;
}

static int32_t context_transform(void* callback_data,
                                 const pi_plugin_callback_context* context,
                                 const char* input_json, size_t input_size,
                                 const char** output_json,
                                 size_t* output_size) {
  (void)context;
  (void)input_json;
  (void)input_size;
  extension_state* extension = (extension_state*)callback_data;
  static const char transform[] =
      "{\"messages\":[{\"role\":\"user\",\"content\":\"This provider context was returned directly by a C hook.\"}]}";
  if (extension == NULL || output_json == NULL || output_size == NULL ||
      extension->closed) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  *output_json = transform;
  *output_size = sizeof(transform) - 1u;
  return PI_PLUGIN_OK;
}

static int32_t after_demo_tool(void* callback_data,
                               const pi_plugin_callback_context* context,
                               const char* input_json, size_t input_size,
                               const char** output_json,
                               size_t* output_size) {
  (void)context;
  extension_state* extension = (extension_state*)callback_data;
  static const char transformed[] =
      "{\"content\":[{\"type\":\"text\",\"text\":\"C tool result returned directly by a C tool_result hook\"}],"
      "\"details\":{\"transformedBy\":\"examples/pi_extension.c\"},\"isError\":false}";
  if (extension == NULL || input_json == NULL || output_json == NULL ||
      output_size == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  static const char tool_name[] = "c_extension_demo";
  int matched = 0;
  if (input_size >= sizeof(tool_name) - 1u) {
    for (size_t i = 0u; i + sizeof(tool_name) - 1u <= input_size; ++i) {
      if (memcmp(input_json + i, tool_name, sizeof(tool_name) - 1u) == 0) {
        matched = 1;
        break;
      }
    }
  }
  if (matched) {
    *output_json = transformed;
    *output_size = sizeof(transformed) - 1u;
  } else {
    *output_json = NULL;
    *output_size = 0u;
  }
  return PI_PLUGIN_OK;
}

static int32_t close_extension(pi_plugin_api* api) {
  extension_state* extension = (extension_state*)api;
  if (extension == NULL || extension->closed) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  extension->closed = 1;
  return PI_PLUGIN_OK;
}

pi_plugin_api* pi_plugin_init(const pi_plugin_host_api* host) {
  if (host == NULL || host->register_tool == NULL ||
      host->register_command == NULL || host->on == NULL ||
      host->is_cancelled == NULL || host->tool_update == NULL ||
      host->ui_notify == NULL || host->ui_set_status == NULL ||
      host->ui_set_widget == NULL) return NULL;

  memset(&state, 0, sizeof(state));
  pi_plugin_copy_host_api(&state.host, host);
  state.api.close = close_extension;

  static const pi_plugin_tool tool = {
      .name = "c_extension_demo",
      .label = "C Extension Demo",
      .description = "Execute a Pi tool registered and implemented by C",
      .parameters_json =
          "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
          "\"required\":[\"name\"],\"additionalProperties\":false}",
      .execute = demo_tool,
      .callback_data = &state,
  };
  static const pi_plugin_command command = {
      .name = "c-extension-demo",
      .description = "Run a C-authored Pi command",
      .execute = demo_command,
      .callback_data = &state,
  };
  static const pi_plugin_event_handler before = {
      .event_name = "before_agent_start",
      .execute = before_agent,
      .callback_data = &state,
  };
  static const pi_plugin_event_handler context = {
      .event_name = "context",
      .execute = context_transform,
      .callback_data = &state,
  };
  static const pi_plugin_event_handler after = {
      .event_name = "tool_result",
      .execute = after_demo_tool,
      .callback_data = &state,
  };

  if (host->register_tool(host->host_data, &tool) != PI_PLUGIN_OK ||
      host->register_command(host->host_data, &command) != PI_PLUGIN_OK ||
      host->on(host->host_data, &before) != PI_PLUGIN_OK ||
      host->on(host->host_data, &context) != PI_PLUGIN_OK ||
      host->on(host->host_data, &after) != PI_PLUGIN_OK) return NULL;
  return &state.api;
}
