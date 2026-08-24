#include "pi_plugin.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct plugin_generation {
  pi_plugin_host_api host;
  size_t call_count;
  pi_plugin_tool tool;
  pi_plugin_command command;
  pi_plugin_event_handler event;
} plugin_generation;

typedef struct plugin_api {
  pi_plugin_api base;
  plugin_generation* generation;
} plugin_api;

static const char* kManifest =
    "{\"name\":\"pi-cplugins-conformance-fixture\",\"call_surface\":\"bounded-byte\"}";

static const char* plugin_manifest(const pi_plugin_api* api,
                                   size_t* byte_count) {
  (void)api;
  if (byte_count != NULL) {
    *byte_count = strlen(kManifest);
  }
  return kManifest;
}

static int32_t plugin_call(pi_plugin_api* api, const uint8_t* input,
                           size_t input_size, uint8_t* output,
                           size_t output_capacity, size_t* output_size) {
  if (api == NULL || output_size == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  plugin_api* plugin = (plugin_api*)api;
  if (plugin->generation == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  if (input_size > api->max_payload_bytes) {
    return PI_PLUGIN_ERR_PAYLOAD_TOO_LARGE;
  }

  if (input_size > 0u && input == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  *output_size = input_size;
  if (output_capacity < input_size) {
    return PI_PLUGIN_ERR_BUFFER_TOO_SMALL;
  }

  if (input_size > 0u && output == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  plugin_generation* generation = plugin->generation;
  if (input_size == 0u) {
    generation->call_count++;
    return PI_PLUGIN_OK;
  }

  uint8_t* scratch =
      (uint8_t*)generation->host.malloc(generation->host.host_data, input_size);
  if (scratch == NULL) {
    return PI_PLUGIN_ERR_OOM;
  }

  for (size_t i = 0; i < input_size; ++i) {
    scratch[i] = (uint8_t)toupper((unsigned char)input[i]);
  }

  memcpy(output, scratch, input_size);
  generation->host.free(generation->host.host_data, scratch);
  generation->call_count++;
  return PI_PLUGIN_OK;
}

static int32_t extension_callback(
    void* callback_data, const pi_plugin_callback_context* context,
    const char* input_json, size_t input_size, const char** output_json,
    size_t* output_size) {
  (void)input_json;
  (void)input_size;
  static const char result[] = "{}";
  if (callback_data == NULL || context == NULL || output_json == NULL ||
      output_size == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  *output_json = result;
  *output_size = sizeof(result) - 1u;
  return PI_PLUGIN_OK;
}

static int32_t plugin_close(pi_plugin_api* api) {
  if (api == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  plugin_api* plugin = (plugin_api*)api;
  plugin_generation* generation = plugin->generation;
  if (generation == NULL) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  pi_plugin_host_api host = generation->host;
  host.free(host.host_data, generation);
  host.free(host.host_data, plugin);
  return PI_PLUGIN_OK;
}

pi_plugin_api* pi_plugin_init(const pi_plugin_host_api* host) {
  if (host == NULL || host->malloc == NULL || host->free == NULL ||
      host->register_tool == NULL || host->register_command == NULL ||
      host->on == NULL) {
    return NULL;
  }

  plugin_generation* generation =
      (plugin_generation*)host->malloc(host->host_data,
                                       sizeof(plugin_generation));
  plugin_api* plugin =
      (plugin_api*)host->malloc(host->host_data, sizeof(plugin_api));
  if (generation == NULL || plugin == NULL) {
    if (generation != NULL) {
      host->free(host->host_data, generation);
    }
    if (plugin != NULL) {
      host->free(host->host_data, plugin);
    }
    return NULL;
  }

  memset(generation, 0, sizeof(*generation));
  memset(plugin, 0, sizeof(*plugin));

  pi_plugin_copy_host_api(&generation->host, host);
  plugin->base.max_payload_bytes = PI_PLUGIN_MAX_CALL_BYTES;
  plugin->base.manifest = plugin_manifest;
  plugin->base.call = plugin_call;
  plugin->base.close = plugin_close;
  plugin->generation = generation;

  generation->tool = (pi_plugin_tool){
      .name = "fixture_tool",
      .label = "Fixture Tool",
      .description = "Direct-linked C extension tool",
      .parameters_json = "{\"type\":\"object\"}",
      .execute = extension_callback,
      .callback_data = generation,
  };
  generation->command = (pi_plugin_command){
      .name = "fixture-command",
      .description = "Direct-linked C extension command",
      .execute = extension_callback,
      .callback_data = generation,
  };
  generation->event = (pi_plugin_event_handler){
      .event_name = "context",
      .execute = extension_callback,
      .callback_data = generation,
  };
  if (host->register_tool(host->host_data, &generation->tool) != PI_PLUGIN_OK ||
      host->register_command(host->host_data, &generation->command) != PI_PLUGIN_OK ||
      host->on(host->host_data, &generation->event) != PI_PLUGIN_OK) {
    plugin_close(&plugin->base);
    return NULL;
  }

  return &plugin->base;
}
