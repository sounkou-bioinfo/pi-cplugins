#include "pi_plugin.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

typedef struct zlib_extension_state {
  pi_plugin_api api;
  pi_plugin_host_api host;
  char output[768];
  char notice[160];
  uLong checksum;
  size_t text_size;
  int has_result;
  int closed;
} zlib_extension_state;

static zlib_extension_state state;

static const char checksum_schema[] =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\","
    "\"pattern\":\"^[A-Za-z0-9 ._-]{1,120}$\"}},\"required\":[\"text\"],"
    "\"additionalProperties\":false}";

static int extract_text(const char* json, size_t json_size,
                        char* text, size_t text_capacity,
                        size_t* text_size) {
  static const char prefix[] = "\"text\":\"";
  const size_t prefix_size = sizeof(prefix) - 1u;
  for (size_t offset = 0u; offset + prefix_size <= json_size; ++offset) {
    if (memcmp(json + offset, prefix, prefix_size) != 0) continue;
    size_t input = offset + prefix_size;
    size_t output = 0u;
    while (input < json_size && json[input] != '"') {
      const unsigned char value = (unsigned char)json[input++];
      if (output + 1u >= text_capacity ||
          !(value == ' ' || value == '.' || value == '_' || value == '-' ||
            (value >= '0' && value <= '9') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z'))) return 0;
      text[output++] = (char)value;
    }
    if (input >= json_size || output == 0u) return 0;
    text[output] = '\0';
    *text_size = output;
    return 1;
  }
  return 0;
}

static int contains_tool_name(const char* json, size_t json_size) {
  static const char name[] = "c_zlib_crc32";
  const size_t name_size = sizeof(name) - 1u;
  for (size_t offset = 0u; offset + name_size <= json_size; ++offset) {
    if (memcmp(json + offset, name, name_size) == 0) return 1;
  }
  return 0;
}

static int32_t write_checksum_result(zlib_extension_state* extension,
                                     int verified,
                                     const char** output_json,
                                     size_t* output_size) {
  const char* hook_content = verified
      ? ",{\"type\":\"text\",\"text\":\"C tool_result hook verified the zlib result.\"}"
      : "";
  const char* hook_detail = verified
      ? ",\"verifiedBy\":\"examples/zlib_extension.c\""
      : "";
  const int written = snprintf(
      extension->output, sizeof(extension->output),
      "{\"content\":[{\"type\":\"text\",\"text\":\"zlib %s computed CRC32 %lu for %lu bytes\"}%s],"
      "\"details\":{\"implementation\":\"examples/zlib_extension.c\","
      "\"library\":\"zlib\",\"version\":\"%s\",\"crc32\":%lu,\"bytes\":%lu%s},"
      "\"isError\":false}",
      zlibVersion(), (unsigned long)extension->checksum,
      (unsigned long)extension->text_size, hook_content,
      zlibVersion(), (unsigned long)extension->checksum,
      (unsigned long)extension->text_size, hook_detail);
  if (written < 0 || (size_t)written >= sizeof(extension->output)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  *output_json = extension->output;
  *output_size = (size_t)written;
  return PI_PLUGIN_OK;
}

static int32_t checksum_tool(void* callback_data,
                             const pi_plugin_callback_context* context,
                             const char* input_json, size_t input_size,
                             const char** output_json, size_t* output_size) {
  zlib_extension_state* extension = (zlib_extension_state*)callback_data;
  char text[121];
  size_t text_size = 0u;
  if (extension == NULL || context == NULL || input_json == NULL ||
      output_json == NULL || output_size == NULL || extension->closed ||
      !extract_text(input_json, input_size, text, sizeof(text), &text_size)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (extension->host.is_cancelled(
          extension->host.host_data, context->host_call_data)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  static const char update[] =
      "{\"content\":[{\"type\":\"text\",\"text\":\"zlib is computing CRC32 in C\"}]}";
  extension->host.tool_update(extension->host.host_data,
                              context->host_call_data,
                              update, sizeof(update) - 1u);
  static const char notification[] = "C extension linked and called host zlib";
  extension->host.ui_notify(extension->host.host_data,
                            context->host_call_data,
                            notification, sizeof(notification) - 1u,
                            PI_PLUGIN_UI_INFO);
  extension->host.ui_set_status(extension->host.host_data,
                                context->host_call_data,
                                "c-zlib", 6u, "CRC32 ready", 11u);
  static const char widget[] =
      "[\"c_zlib_crc32 is implemented in C\",\"libz supplied the checksum\"]";
  extension->host.ui_set_widget(extension->host.host_data,
                                context->host_call_data,
                                "c-zlib", 6u,
                                widget, sizeof(widget) - 1u,
                                PI_PLUGIN_WIDGET_ABOVE_EDITOR);

  extension->checksum = crc32(0L, (const Bytef*)text, (uInt)text_size);
  extension->text_size = text_size;
  extension->has_result = 1;
  return write_checksum_result(extension, 0, output_json, output_size);
}

static int32_t version_command(void* callback_data,
                               const pi_plugin_callback_context* context,
                               const char* input_json, size_t input_size,
                               const char** output_json, size_t* output_size) {
  zlib_extension_state* extension = (zlib_extension_state*)callback_data;
  (void)input_json;
  (void)input_size;
  if (extension == NULL || context == NULL || output_json == NULL ||
      output_size == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  const int written = snprintf(extension->notice, sizeof(extension->notice),
                               "C extension uses zlib %s", zlibVersion());
  if (written < 0 || (size_t)written >= sizeof(extension->notice)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  extension->host.ui_notify(extension->host.host_data,
                            context->host_call_data,
                            extension->notice, (size_t)written,
                            PI_PLUGIN_UI_INFO);
  *output_json = NULL;
  *output_size = 0u;
  return PI_PLUGIN_OK;
}

static int32_t verify_result(void* callback_data,
                             const pi_plugin_callback_context* context,
                             const char* input_json, size_t input_size,
                             const char** output_json, size_t* output_size) {
  zlib_extension_state* extension = (zlib_extension_state*)callback_data;
  (void)context;
  if (extension == NULL || input_json == NULL || output_json == NULL ||
      output_size == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (contains_tool_name(input_json, input_size) && extension->has_result) {
    return write_checksum_result(extension, 1, output_json, output_size);
  }
  *output_json = NULL;
  *output_size = 0u;
  return PI_PLUGIN_OK;
}

static int32_t close_extension(pi_plugin_api* api) {
  zlib_extension_state* extension = (zlib_extension_state*)api;
  if (extension == NULL || extension->closed) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
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
      .name = "c_zlib_crc32",
      .label = "C zlib CRC32",
      .description = "Compute CRC32 by calling host zlib from a C-authored Pi tool",
      .parameters_json = checksum_schema,
      .execute = checksum_tool,
      .callback_data = &state,
  };
  static const pi_plugin_command command = {
      .name = "c-zlib-version",
      .description = "Show the zlib version linked by the C extension",
      .execute = version_command,
      .callback_data = &state,
  };
  static const pi_plugin_event_handler result_hook = {
      .event_name = "tool_result",
      .execute = verify_result,
      .callback_data = &state,
  };

  if (host->register_tool(host->host_data, &tool) != PI_PLUGIN_OK ||
      host->register_command(host->host_data, &command) != PI_PLUGIN_OK ||
      host->on(host->host_data, &result_hook) != PI_PLUGIN_OK) return NULL;
  return &state.api;
}
