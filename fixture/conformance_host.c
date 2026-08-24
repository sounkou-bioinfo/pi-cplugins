#include "pi_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct counted_allocator {
  size_t alloc_count;
  size_t free_count;
  size_t total_alloc_bytes;
  const pi_plugin_tool* tool;
  const pi_plugin_command* command;
  const pi_plugin_event_handler* event;
} counted_allocator;

static void* host_malloc(void* host_data, size_t size) {
  counted_allocator* state = (counted_allocator*)host_data;
  state->alloc_count++;
  state->total_alloc_bytes += size;
  return malloc(size);
}

static void host_free(void* host_data, void* ptr) {
  counted_allocator* state = (counted_allocator*)host_data;
  state->free_count++;
  free(ptr);
}

static int32_t host_register_tool(void* host_data,
                                  const pi_plugin_tool* tool) {
  counted_allocator* state = (counted_allocator*)host_data;
  if (state == NULL || tool == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  state->tool = tool;
  return PI_PLUGIN_OK;
}

static int32_t host_register_command(void* host_data,
                                     const pi_plugin_command* command) {
  counted_allocator* state = (counted_allocator*)host_data;
  if (state == NULL || command == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  state->command = command;
  return PI_PLUGIN_OK;
}

static int32_t host_register_event(void* host_data,
                                   const pi_plugin_event_handler* event) {
  counted_allocator* state = (counted_allocator*)host_data;
  if (state == NULL || event == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  state->event = event;
  return PI_PLUGIN_OK;
}

static int check(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

int main(void) {
  int failures = 0;

  counted_allocator allocator_state = {0};
  pi_plugin_host_api host = {
    .host_data = &allocator_state,
    .malloc = host_malloc,
    .free = host_free,
    .register_tool = host_register_tool,
    .register_command = host_register_command,
    .on = host_register_event,
  };

  pi_plugin_init_fn init_fn = pi_plugin_init;

  failures += !check(init_fn(NULL) == NULL, "NULL host API must be rejected");

  pi_plugin_host_api bad_host = host;
  bad_host.malloc = NULL;
  failures +=
      !check(init_fn(&bad_host) == NULL, "missing allocator must be rejected");

  pi_plugin_host_api extended_host = host;
  extended_host.reserved[0] = &extended_host;
  pi_plugin_api* extended_api = init_fn(&extended_host);
  failures += !check(extended_api != NULL,
                     "unknown reserved slots must be ignored");
  if (extended_api != NULL) {
    failures += !check(extended_api->close(extended_api) == PI_PLUGIN_OK,
                       "extended host probe must close cleanly");
  }

  pi_plugin_api* api = init_fn(&host);
  if (!check(api != NULL, "plugin registration failed")) {
    return 1;
  }

  failures += !check(allocator_state.tool != NULL &&
                         allocator_state.command != NULL &&
                         allocator_state.event != NULL,
                     "plugin must register C-authored Pi capabilities");
  if (allocator_state.tool != NULL) {
    const pi_plugin_callback_context context = {.cwd = ".", .cwd_size = 1u};
    const char* callback_output = NULL;
    size_t callback_size = 0u;
    const int32_t callback_status = allocator_state.tool->execute(
        allocator_state.tool->callback_data, &context, "{}", 2u,
        &callback_output, &callback_size);
    failures += !check(callback_status == PI_PLUGIN_OK && callback_size == 2u &&
                           callback_output != NULL &&
                           memcmp(callback_output, "{}", 2u) == 0,
                       "registered C tool callback must execute directly");
  }

  failures += !check(api->max_payload_bytes == PI_PLUGIN_MAX_CALL_BYTES,
                     "max payload should match bound");
  failures += !check(api->manifest != NULL && api->call != NULL &&
                         api->close != NULL,
                     "plugin hooks must be present");
  for (size_t i = 0; i < PI_PLUGIN_RESERVED_SLOTS; ++i) {
    failures += !check(api->reserved[i] == NULL,
                       "plugin reserved slots must be NULL");
  }

  size_t manifest_size = 0u;
  const char* manifest = api->manifest(api, &manifest_size);
  failures += !check(manifest != NULL && manifest_size > 0u,
                     "manifest must be readable and non-empty");

  uint8_t output[PI_PLUGIN_MAX_CALL_BYTES] = {0};
  size_t output_size = 0u;
  const char* input = "cplugins";
  const size_t input_size = strlen(input);

  int32_t rc = api->call(api, (const uint8_t*)input, input_size, output,
                         input_size - 1u, &output_size);
  failures += !check(rc == PI_PLUGIN_ERR_BUFFER_TOO_SMALL &&
                         output_size == input_size,
                     "small output buffer must report required capacity");

  rc = api->call(api, NULL, 1u, output, sizeof(output), &output_size);
  failures += !check(rc == PI_PLUGIN_ERR_INVALID_ARGUMENT,
                     "non-empty NULL input must be rejected");

  uint8_t oversized[PI_PLUGIN_MAX_CALL_BYTES + 1u] = {0};
  rc = api->call(api, oversized, sizeof(oversized), output, sizeof(output),
                 &output_size);
  failures += !check(rc == PI_PLUGIN_ERR_PAYLOAD_TOO_LARGE,
                     "payload larger than bound must be rejected");

  rc = api->call(api, (const uint8_t*)input, input_size, output,
                 sizeof(output), &output_size);
  failures += !check(rc == PI_PLUGIN_OK,
                     "bounded call for small input should succeed");
  failures += !check(output_size == input_size &&
                         memcmp(output, "CPLUGINS", input_size) == 0,
                     "call output must be deterministic");

  rc = api->call(api, NULL, 0u, NULL, 0u, &output_size);
  failures += !check(rc == PI_PLUGIN_OK && output_size == 0u,
                     "empty bounded call should succeed");

  rc = api->close(api);
  failures += !check(rc == PI_PLUGIN_OK, "close should succeed");
  failures += !check(allocator_state.alloc_count == allocator_state.free_count,
                     "allocator ownership must balance at close");

  printf("conformance: %s (alloc=%zu free=%zu bytes=%zu)\n",
         failures == 0 ? "OK" : "FAILED", allocator_state.alloc_count,
         allocator_state.free_count, allocator_state.total_alloc_bytes);

  return failures == 0 ? 0 : 1;
}
