#include "pi_native_plugin.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct pi_native_call {
  napi_env env;
  napi_value bridge;
} pi_native_call;

static int valid_name(const char* name) {
  if (name == NULL || !((*name >= 'A' && *name <= 'Z') ||
                        (*name >= 'a' && *name <= 'z') || *name == '_')) return 0;
  for (const char* cursor = name + 1; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '_' ||
          *cursor == '-')) return 0;
  }
  return 1;
}

static int register_callback(compiled_plugin* plugin,
                             pi_native_registration_kind kind,
                             const char* name, const char* label,
                             const char* description,
                             const char* parameters_json,
                             pi_plugin_callback_fn execute,
                             void* callback_data) {
  if (plugin == NULL || !valid_name(name) || execute == NULL ||
      plugin->registration_count >= PI_NATIVE_MAX_REGISTRATIONS ||
      (kind == PI_NATIVE_TOOL &&
       (label == NULL || description == NULL || parameters_json == NULL))) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  pi_native_registration* registration =
      &plugin->registrations[plugin->registration_count++];
  registration->kind = kind;
  registration->name = name;
  registration->label = label;
  registration->description = description;
  registration->parameters_json = parameters_json;
  registration->execute = execute;
  registration->callback_data = callback_data;
  return PI_PLUGIN_OK;
}

static int32_t register_tool(void* host_data, const pi_plugin_tool* tool) {
  if (tool == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  return register_callback((compiled_plugin*)host_data, PI_NATIVE_TOOL,
                           tool->name, tool->label, tool->description,
                           tool->parameters_json, tool->execute,
                           tool->callback_data);
}

static int32_t register_command(void* host_data,
                                const pi_plugin_command* command) {
  if (command == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  return register_callback((compiled_plugin*)host_data, PI_NATIVE_COMMAND,
                           command->name, NULL, command->description, NULL,
                           command->execute, command->callback_data);
}

static int32_t register_event(void* host_data,
                              const pi_plugin_event_handler* handler) {
  if (handler == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  return register_callback((compiled_plugin*)host_data, PI_NATIVE_EVENT,
                           handler->event_name, NULL, NULL, NULL,
                           handler->execute, handler->callback_data);
}

static int bridge_call(pi_native_call* call, const char* method_name,
                       size_t argument_count, napi_value* arguments,
                       napi_value* result) {
  if (call == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  napi_value method;
  napi_valuetype type;
  if (napi_get_named_property(call->env, call->bridge, method_name, &method) != napi_ok ||
      napi_typeof(call->env, method, &type) != napi_ok || type != napi_function ||
      napi_call_function(call->env, call->bridge, method, argument_count,
                         arguments, result) != napi_ok) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  return PI_PLUGIN_OK;
}

static int make_string(napi_env env, const char* text, size_t size,
                       napi_value* result) {
  return text != NULL && size <= PI_PLUGIN_MAX_JSON_BYTES &&
         napi_create_string_utf8(env, text, size, result) == napi_ok;
}

static int32_t host_is_cancelled(void* host_data, void* host_call_data) {
  (void)host_data;
  pi_native_call* call = (pi_native_call*)host_call_data;
  napi_value result;
  bool cancelled = true;
  if (bridge_call(call, "isCancelled", 0u, NULL, &result) != PI_PLUGIN_OK ||
      napi_get_value_bool(call->env, result, &cancelled) != napi_ok) return 1;
  return cancelled ? 1 : 0;
}

static int32_t host_tool_update(void* host_data, void* host_call_data,
                                const char* update_json, size_t update_size) {
  (void)host_data;
  pi_native_call* call = (pi_native_call*)host_call_data;
  napi_value argument;
  napi_value result;
  if (call == NULL || !make_string(call->env, update_json, update_size, &argument)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  return bridge_call(call, "update", 1u, &argument, &result);
}

static int32_t host_ui_notify(void* host_data, void* host_call_data,
                              const char* message, size_t message_size,
                              int32_t level) {
  (void)host_data;
  pi_native_call* call = (pi_native_call*)host_call_data;
  napi_value arguments[2];
  napi_value result;
  if (call == NULL || !make_string(call->env, message, message_size, &arguments[0]) ||
      napi_create_int32(call->env, level, &arguments[1]) != napi_ok) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  return bridge_call(call, "notify", 2u, arguments, &result);
}

static int32_t host_ui_set_status(void* host_data, void* host_call_data,
                                  const char* key, size_t key_size,
                                  const char* text, size_t text_size) {
  (void)host_data;
  pi_native_call* call = (pi_native_call*)host_call_data;
  napi_value arguments[2];
  napi_value result;
  if (call == NULL || !make_string(call->env, key, key_size, &arguments[0])) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (text == NULL) {
    if (napi_get_null(call->env, &arguments[1]) != napi_ok) {
      return PI_PLUGIN_ERR_INVALID_ARGUMENT;
    }
  } else if (!make_string(call->env, text, text_size, &arguments[1])) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  return bridge_call(call, "setStatus", 2u, arguments, &result);
}

static int32_t host_ui_set_widget(void* host_data, void* host_call_data,
                                  const char* key, size_t key_size,
                                  const char* lines_json, size_t lines_size,
                                  int32_t placement) {
  (void)host_data;
  pi_native_call* call = (pi_native_call*)host_call_data;
  napi_value arguments[3];
  napi_value result;
  if (call == NULL || !make_string(call->env, key, key_size, &arguments[0])) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (lines_json == NULL) {
    if (napi_get_null(call->env, &arguments[1]) != napi_ok) {
      return PI_PLUGIN_ERR_INVALID_ARGUMENT;
    }
  } else if (!make_string(call->env, lines_json, lines_size, &arguments[1])) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  if (napi_create_int32(call->env, placement, &arguments[2]) != napi_ok) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  return bridge_call(call, "setWidget", 3u, arguments, &result);
}

void pi_native_host_api(compiled_plugin* plugin, pi_plugin_host_api* host) {
  memset(host, 0, sizeof(*host));
  host->host_data = plugin;
  host->register_tool = register_tool;
  host->register_command = register_command;
  host->on = register_event;
  host->is_cancelled = host_is_cancelled;
  host->tool_update = host_tool_update;
  host->ui_notify = host_ui_notify;
  host->ui_set_status = host_ui_set_status;
  host->ui_set_widget = host_ui_set_widget;
}

static compiled_plugin* unwrap(napi_env env, napi_value self) {
  compiled_plugin* plugin = NULL;
  if (napi_unwrap(env, self, (void**)&plugin) != napi_ok || plugin == NULL ||
      plugin->api == NULL || plugin->compiler == NULL) {
    napi_throw_error(env, NULL, "TinyCC plugin is closed");
    return NULL;
  }
  return plugin;
}

static int set_string_property(napi_env env, napi_value object,
                               const char* name, const char* value) {
  napi_value string;
  return value != NULL &&
         napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &string) == napi_ok &&
         napi_set_named_property(env, object, name, string) == napi_ok;
}

napi_value pi_native_registrations(napi_env env, napi_callback_info info) {
  napi_value self;
  size_t argc = 0u;
  if (napi_get_cb_info(env, info, &argc, NULL, &self, NULL) != napi_ok) return NULL;
  compiled_plugin* plugin = unwrap(env, self);
  if (plugin == NULL) return NULL;

  napi_value registrations;
  if (napi_create_array_with_length(env, plugin->registration_count,
                                    &registrations) != napi_ok) return NULL;
  for (size_t i = 0u; i < plugin->registration_count; ++i) {
    const pi_native_registration* registration = &plugin->registrations[i];
    const char* kind = registration->kind == PI_NATIVE_TOOL ? "tool" :
                       registration->kind == PI_NATIVE_COMMAND ? "command" : "event";
    napi_value object;
    napi_value id;
    if (napi_create_object(env, &object) != napi_ok ||
        napi_create_uint32(env, (uint32_t)i, &id) != napi_ok ||
        napi_set_named_property(env, object, "id", id) != napi_ok ||
        !set_string_property(env, object, "kind", kind) ||
        !set_string_property(env, object, "name", registration->name)) return NULL;
    if (registration->label != NULL &&
        !set_string_property(env, object, "label", registration->label)) return NULL;
    if (registration->description != NULL &&
        !set_string_property(env, object, "description",
                             registration->description)) return NULL;
    if (registration->parameters_json != NULL &&
        !set_string_property(env, object, "parametersJson",
                             registration->parameters_json)) return NULL;
    if (napi_set_element(env, registrations, (uint32_t)i, object) != napi_ok) return NULL;
  }
  return registrations;
}

napi_value pi_native_invoke(napi_env env, napi_callback_info info) {
  napi_value arguments[4];
  napi_value self;
  size_t argc = 4u;
  if (napi_get_cb_info(env, info, &argc, arguments, &self, NULL) != napi_ok ||
      argc != 4u) {
    napi_throw_type_error(env, NULL, "invoke expects registration id, input JSON, cwd, and bridge");
    return NULL;
  }
  compiled_plugin* plugin = unwrap(env, self);
  uint32_t id;
  if (plugin == NULL || napi_get_value_uint32(env, arguments[0], &id) != napi_ok ||
      id >= plugin->registration_count) {
    napi_throw_type_error(env, NULL, "invalid C extension registration id");
    return NULL;
  }

  size_t input_size = 0u;
  size_t cwd_size = 0u;
  if (napi_get_value_string_utf8(env, arguments[1], NULL, 0u, &input_size) != napi_ok ||
      napi_get_value_string_utf8(env, arguments[2], NULL, 0u, &cwd_size) != napi_ok ||
      input_size > PI_PLUGIN_MAX_JSON_BYTES || cwd_size > PI_PLUGIN_MAX_JSON_BYTES) {
    napi_throw_type_error(env, NULL, "C extension input or cwd is invalid");
    return NULL;
  }
  char* input = (char*)malloc(input_size + 1u);
  char* cwd = (char*)malloc(cwd_size + 1u);
  if (input == NULL || cwd == NULL ||
      napi_get_value_string_utf8(env, arguments[1], input, input_size + 1u,
                                 &input_size) != napi_ok ||
      napi_get_value_string_utf8(env, arguments[2], cwd, cwd_size + 1u,
                                 &cwd_size) != napi_ok) {
    free(input);
    free(cwd);
    napi_throw_error(env, NULL, "could not copy C extension invocation");
    return NULL;
  }

  pi_native_call call = {.env = env, .bridge = arguments[3]};
  pi_plugin_callback_context context;
  memset(&context, 0, sizeof(context));
  context.host_call_data = &call;
  context.cwd = cwd;
  context.cwd_size = cwd_size;
  const char* output = NULL;
  size_t output_size = 0u;
  const pi_native_registration* registration = &plugin->registrations[id];
  const int32_t status = registration->execute(
      registration->callback_data, &context, input, input_size, &output,
      &output_size);
  free(input);
  free(cwd);
  if (status != PI_PLUGIN_OK || output_size > PI_PLUGIN_MAX_JSON_BYTES ||
      (output_size > 0u && output == NULL)) {
    napi_throw_error(env, NULL, "C extension callback failed");
    return NULL;
  }
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || pending) return NULL;
  napi_value result;
  if (output == NULL) napi_get_undefined(env, &result);
  else if (napi_create_string_utf8(env, output, output_size, &result) != napi_ok) return NULL;
  return result;
}
