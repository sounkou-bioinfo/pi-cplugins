#include <node_api.h>

#include "pi_native_plugin.h"
#include "pi_typed_node.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PI_TCC_LIB_PATH
#error "PI_TCC_LIB_PATH must point to TinyCC's runtime library directory"
#endif

#define PI_TCC_MAX_SOURCE_BYTES (4u * 1024u * 1024u)
#define PI_TCC_MAX_OUTPUT_BYTES (16u * 1024u * 1024u)
#define PI_TCC_MAX_MANIFEST_BYTES (1u * 1024u * 1024u)
#define PI_TCC_ERROR_BYTES 4096u

static napi_value throw_message(napi_env env, const char* message) {
  napi_throw_error(env, NULL, message);
  return NULL;
}

static napi_value throw_status(napi_env env, const char* operation,
                               int32_t status) {
  char message[128];
  snprintf(message, sizeof(message), "%s failed with plugin status %d",
           operation, (int)status);
  return throw_message(env, message);
}

static void tcc_error(void* user_data, const char* message) {
  compile_errors* errors = (compile_errors*)user_data;
  if (errors == NULL || message == NULL || errors->length >= sizeof(errors->text) - 1u) {
    return;
  }

  const size_t available = sizeof(errors->text) - errors->length - 1u;
  const size_t message_size = strlen(message);
  const size_t copied = message_size < available ? message_size : available;
  memcpy(errors->text + errors->length, message, copied);
  errors->length += copied;
  if (errors->length < sizeof(errors->text) - 1u) {
    errors->text[errors->length++] = '\n';
  }
  errors->text[errors->length] = '\0';
}

static void* plugin_host_malloc(void* host_data, size_t size) {
  (void)host_data;
  return malloc(size == 0u ? 1u : size);
}

static void plugin_host_free(void* host_data, void* ptr) {
  (void)host_data;
  free(ptr);
}

static int release_plugin(compiled_plugin* plugin) {
  if (plugin == NULL) return 1;

  if (plugin->api != NULL) {
    if (plugin->api->close == NULL ||
        plugin->api->close(plugin->api) != PI_PLUGIN_OK) {
      return 0;
    }
    plugin->api = NULL;
  }
  if (plugin->compiler != NULL) {
    tcc_delete(plugin->compiler);
    plugin->compiler = NULL;
  }
  return 1;
}

static void destroy_plugin(compiled_plugin* plugin) {
  if (release_plugin(plugin)) free(plugin);
}

static void finalize_plugin(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  destroy_plugin((compiled_plugin*)data);
}

static compiled_plugin* unwrap_plugin(napi_env env, napi_callback_info info) {
  napi_value self;
  size_t argc = 0u;
  if (napi_get_cb_info(env, info, &argc, NULL, &self, NULL) != napi_ok) {
    throw_message(env, "could not read TinyCC plugin receiver");
    return NULL;
  }

  compiled_plugin* plugin = NULL;
  if (napi_unwrap(env, self, (void**)&plugin) != napi_ok || plugin == NULL) {
    throw_message(env, "invalid TinyCC plugin receiver");
    return NULL;
  }
  if (plugin->api == NULL || plugin->compiler == NULL) {
    throw_message(env, "TinyCC plugin is closed");
    return NULL;
  }
  return plugin;
}

static napi_value plugin_manifest(napi_env env, napi_callback_info info) {
  compiled_plugin* plugin = unwrap_plugin(env, info);
  if (plugin == NULL) {
    return NULL;
  }

  if (plugin->api->manifest == NULL) {
    return throw_message(env, "plugin does not expose a byte-call manifest");
  }
  size_t byte_count = 0u;
  const char* manifest = plugin->api->manifest(plugin->api, &byte_count);
  if (manifest == NULL || byte_count > PI_TCC_MAX_MANIFEST_BYTES) {
    return throw_message(env, "plugin returned an invalid manifest");
  }

  napi_value result;
  if (napi_create_string_utf8(env, manifest, byte_count, &result) != napi_ok) {
    return throw_message(env, "could not copy plugin manifest");
  }
  return result;
}

static napi_value plugin_call(napi_env env, napi_callback_info info) {
  napi_value args[1];
  napi_value self;
  size_t argc = 1u;
  if (napi_get_cb_info(env, info, &argc, args, &self, NULL) != napi_ok || argc != 1u) {
    return throw_message(env, "call expects one Buffer argument");
  }

  compiled_plugin* plugin = NULL;
  if (napi_unwrap(env, self, (void**)&plugin) != napi_ok || plugin == NULL ||
      plugin->api == NULL || plugin->compiler == NULL) {
    return throw_message(env, "TinyCC plugin is closed");
  }
  if (plugin->api->call == NULL) {
    return throw_message(env, "plugin does not expose the bounded byte-call capability");
  }

  bool is_buffer = false;
  if (napi_is_buffer(env, args[0], &is_buffer) != napi_ok || !is_buffer) {
    return throw_message(env, "call expects one Buffer argument");
  }

  void* input_data = NULL;
  size_t input_size = 0u;
  if (napi_get_buffer_info(env, args[0], &input_data, &input_size) != napi_ok) {
    return throw_message(env, "could not read call input");
  }
  if (input_size > plugin->api->max_payload_bytes) {
    return throw_message(env, "call input exceeds the plugin payload bound");
  }
  if (plugin->api->max_payload_bytes > PI_TCC_MAX_OUTPUT_BYTES) {
    return throw_message(env, "plugin output bound exceeds the host safety limit");
  }

  const size_t output_capacity = plugin->api->max_payload_bytes;
  uint8_t* output = (uint8_t*)malloc(output_capacity);
  if (output == NULL) {
    return throw_message(env, "could not allocate call output");
  }

  size_t output_size = 0u;
  const int32_t status = plugin->api->call(
      plugin->api, (const uint8_t*)input_data, input_size, output,
      output_capacity, &output_size);
  if (status == PI_PLUGIN_ERR_BUFFER_TOO_SMALL) {
    free(output);
    return throw_message(env, "plugin output exceeds its declared payload bound");
  }
  if (status != PI_PLUGIN_OK) {
    free(output);
    return throw_status(env, "call", status);
  }
  if (output_size > output_capacity) {
    free(output);
    return throw_message(env, "plugin wrote an invalid output size");
  }

  napi_value result;
  const void* copied_from =
      output_size == 0u ? (const void*)"" : (const void*)output;
  if (napi_create_buffer_copy(env, output_size, copied_from, NULL, &result) != napi_ok) {
    free(output);
    return throw_message(env, "could not copy call output");
  }
  free(output);
  return result;
}

static napi_value plugin_symbol(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  napi_value self;
  size_t argc = 1u;
  if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok ||
      argc != 1u) {
    return throw_message(env, "symbol expects one C symbol name");
  }

  compiled_plugin* plugin = NULL;
  if (napi_unwrap(env, self, (void**)&plugin) != napi_ok || plugin == NULL ||
      plugin->api == NULL || plugin->compiler == NULL) {
    return throw_message(env, "TinyCC plugin is closed");
  }

  size_t name_size = 0u;
  if (napi_get_value_string_utf8(env, argv[0], NULL, 0u, &name_size) != napi_ok ||
      name_size == 0u || name_size > 255u) {
    return throw_message(env, "symbol name must be a non-empty string");
  }
  char name[256];
  if (napi_get_value_string_utf8(env, argv[0], name, sizeof(name),
                                 &name_size) != napi_ok) {
    return throw_message(env, "could not copy symbol name");
  }

  void* symbol = tcc_get_symbol(plugin->compiler, name);
  if (symbol == NULL || (double)(uintptr_t)symbol > 9007199254740991.0) {
    return throw_message(env, "compiled C symbol is missing or not representable");
  }
  napi_value result;
  if (napi_create_double(env, (double)(uintptr_t)symbol, &result) != napi_ok) {
    return throw_message(env, "could not return compiled C symbol");
  }
  return result;
}

static napi_value plugin_close(napi_env env, napi_callback_info info) {
  compiled_plugin* plugin = unwrap_plugin(env, info);
  if (plugin == NULL) {
    return NULL;
  }

  const int32_t status = plugin->api->close(plugin->api);
  if (status != PI_PLUGIN_OK) {
    return throw_status(env, "close", status);
  }
  plugin->api = NULL;
  tcc_delete(plugin->compiler);
  plugin->compiler = NULL;

  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static int add_include_paths(napi_env env, TCCState* compiler,
                             napi_value paths) {
  bool is_array = false;
  if (napi_is_array(env, paths, &is_array) != napi_ok || !is_array) {
    throw_message(env, "compile includePaths must be an array");
    return 0;
  }

  uint32_t path_count = 0u;
  if (napi_get_array_length(env, paths, &path_count) != napi_ok) {
    throw_message(env, "could not read compile includePaths");
    return 0;
  }

  for (uint32_t i = 0u; i < path_count; ++i) {
    napi_value value;
    if (napi_get_element(env, paths, i, &value) != napi_ok) {
      throw_message(env, "could not read an include path");
      return 0;
    }

    size_t path_size = 0u;
    if (napi_get_value_string_utf8(env, value, NULL, 0u, &path_size) != napi_ok) {
      throw_message(env, "every include path must be a string");
      return 0;
    }

    char* path = (char*)malloc(path_size + 1u);
    if (path == NULL) {
      throw_message(env, "could not allocate an include path");
      return 0;
    }
    if (napi_get_value_string_utf8(env, value, path, path_size + 1u,
                                   &path_size) != napi_ok) {
      free(path);
      throw_message(env, "could not copy an include path");
      return 0;
    }
    const int status = tcc_add_include_path(compiler, path);
    free(path);
    if (status < 0) {
      throw_message(env, "TinyCC rejected an include path");
      return 0;
    }
  }
  return 1;
}

static char* copy_option_string(napi_env env, napi_value value,
                                int allow_empty) {
  size_t size = 0u;
  if (napi_get_value_string_utf8(env, value, NULL, 0u, &size) != napi_ok ||
      (!allow_empty && size == 0u) || size > 4096u) {
    throw_message(env, "TinyCC options must be non-empty strings up to 4096 bytes");
    return NULL;
  }
  char* result = (char*)malloc(size + 1u);
  if (result == NULL ||
      napi_get_value_string_utf8(env, value, result, size + 1u, &size) != napi_ok) {
    free(result);
    throw_message(env, "could not copy TinyCC option");
    return NULL;
  }
  return result;
}

static int apply_flags(napi_env env, TCCState* compiler, napi_value values) {
  uint32_t length = 0u;
  bool is_array = false;
  if (napi_is_array(env, values, &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, values, &length) != napi_ok || length > 128u) {
    throw_message(env, "TinyCC flags must be an array with at most 128 entries");
    return 0;
  }
  for (uint32_t i = 0u; i < length; ++i) {
    napi_value value;
    if (napi_get_element(env, values, i, &value) != napi_ok) return 0;
    char* flag = copy_option_string(env, value, 0);
    if (flag == NULL) return 0;
    const int status = tcc_set_options(compiler, flag);
    free(flag);
    if (status < 0) {
      throw_message(env, "TinyCC rejected a compiler flag");
      return 0;
    }
  }
  return 1;
}

static int apply_defines(napi_env env, TCCState* compiler, napi_value values) {
  uint32_t length = 0u;
  bool is_array = false;
  if (napi_is_array(env, values, &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, values, &length) != napi_ok || length > 128u) {
    throw_message(env, "TinyCC defines must be an array with at most 128 entries");
    return 0;
  }
  for (uint32_t i = 0u; i < length; ++i) {
    napi_value pair;
    bool pair_is_array = false;
    uint32_t pair_length = 0u;
    if (napi_get_element(env, values, i, &pair) != napi_ok ||
        napi_is_array(env, pair, &pair_is_array) != napi_ok || !pair_is_array ||
        napi_get_array_length(env, pair, &pair_length) != napi_ok || pair_length != 2u) {
      throw_message(env, "each TinyCC define must be a name/value pair");
      return 0;
    }
    napi_value name_value;
    napi_value definition_value;
    if (napi_get_element(env, pair, 0u, &name_value) != napi_ok ||
        napi_get_element(env, pair, 1u, &definition_value) != napi_ok) return 0;
    char* name = copy_option_string(env, name_value, 0);
    if (name == NULL) return 0;
    char* definition = copy_option_string(env, definition_value, 1);
    if (definition == NULL) {
      free(name);
      return 0;
    }
    tcc_define_symbol(compiler, name, definition);
    free(name);
    free(definition);
  }
  return 1;
}

static int apply_libraries(napi_env env, TCCState* compiler,
                           napi_value values) {
  uint32_t length = 0u;
  bool is_array = false;
  if (napi_is_array(env, values, &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, values, &length) != napi_ok || length > 128u) {
    throw_message(env, "TinyCC libraries must be an array with at most 128 entries");
    return 0;
  }
  for (uint32_t i = 0u; i < length; ++i) {
    napi_value value;
    if (napi_get_element(env, values, i, &value) != napi_ok) return 0;
    char* library = copy_option_string(env, value, 0);
    if (library == NULL) return 0;
    const int status = tcc_add_library(compiler, library);
    free(library);
    if (status < 0) {
      throw_message(env, "TinyCC could not link a requested library");
      return 0;
    }
  }
  return 1;
}

static napi_value compile_plugin(napi_env env, napi_callback_info info) {
  napi_value args[5];
  size_t argc = 5u;
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc != 5u) {
    return throw_message(
        env, "compile expects source, includePaths, flags, libraries, and defines");
  }

  size_t source_size = 0u;
  if (napi_get_value_string_utf8(env, args[0], NULL, 0u, &source_size) != napi_ok) {
    return throw_message(env, "compile source must be a string");
  }
  if (source_size == 0u || source_size > PI_TCC_MAX_SOURCE_BYTES) {
    return throw_message(env, "compile source is empty or exceeds 4 MiB");
  }

  char* source = (char*)malloc(source_size + 1u);
  if (source == NULL) {
    return throw_message(env, "could not allocate compile source");
  }
  if (napi_get_value_string_utf8(env, args[0], source, source_size + 1u,
                                 &source_size) != napi_ok) {
    free(source);
    return throw_message(env, "could not copy compile source");
  }

  compiled_plugin* plugin = (compiled_plugin*)calloc(1u, sizeof(*plugin));
  if (plugin == NULL) {
    free(source);
    return throw_message(env, "could not allocate TinyCC plugin state");
  }

  plugin->compiler = tcc_new();
  if (plugin->compiler == NULL) {
    free(source);
    free(plugin);
    return throw_message(env, "tcc_new failed");
  }
  tcc_set_lib_path(plugin->compiler, PI_TCC_LIB_PATH);
  tcc_set_error_func(plugin->compiler, &plugin->errors, tcc_error);

  if (!apply_flags(env, plugin->compiler, args[2]) ||
      !apply_defines(env, plugin->compiler, args[4])) {
    free(source);
    destroy_plugin(plugin);
    return NULL;
  }
  if (tcc_set_output_type(plugin->compiler, TCC_OUTPUT_MEMORY) < 0) {
    napi_value failure = throw_message(
        env, plugin->errors.length > 0u ? plugin->errors.text
                                        : "TinyCC output setup failed");
    free(source);
    destroy_plugin(plugin);
    return failure;
  }
  if (!add_include_paths(env, plugin->compiler, args[1])) {
    free(source);
    destroy_plugin(plugin);
    return NULL;
  }

  if (tcc_compile_string(plugin->compiler, source) < 0) {
    napi_value failure = throw_message(env, plugin->errors.length > 0u
                                               ? plugin->errors.text
                                               : "TinyCC compilation failed");
    free(source);
    destroy_plugin(plugin);
    return failure;
  }
  free(source);

  if (!apply_libraries(env, plugin->compiler, args[3])) {
    destroy_plugin(plugin);
    return NULL;
  }
  if (tcc_relocate(plugin->compiler) < 0) {
    napi_value failure = throw_message(env, plugin->errors.length > 0u
                                               ? plugin->errors.text
                                               : "TinyCC relocation failed");
    destroy_plugin(plugin);
    return failure;
  }

  void* symbol = tcc_get_symbol(plugin->compiler, "pi_plugin_init");
  pi_plugin_init_fn init = NULL;
  if (symbol == NULL || sizeof(init) != sizeof(symbol)) {
    destroy_plugin(plugin);
    return throw_message(env, "compiled source does not export pi_plugin_init");
  }
  memcpy(&init, &symbol, sizeof(init));

  pi_plugin_host_api host;
  pi_native_host_api(plugin, &host);
  host.malloc = plugin_host_malloc;
  host.free = plugin_host_free;
  plugin->api = init(&host);
  if (plugin->api == NULL || plugin->api->close == NULL ||
      (plugin->api->call != NULL &&
       (plugin->api->max_payload_bytes == 0u ||
        plugin->api->max_payload_bytes > PI_TCC_MAX_OUTPUT_BYTES))) {
    if (plugin->api != NULL && plugin->api->close == NULL) plugin->api = NULL;
    destroy_plugin(plugin);
    return throw_message(env, "pi_plugin_init returned an invalid plugin API");
  }

  napi_value result;
  if (napi_create_object(env, &result) != napi_ok) {
    destroy_plugin(plugin);
    return throw_message(env, "could not create TinyCC plugin object");
  }
  const napi_property_descriptor methods[] = {
      {"manifest", NULL, plugin_manifest, NULL, NULL, NULL, napi_default, NULL},
      {"call", NULL, plugin_call, NULL, NULL, NULL, napi_default, NULL},
      {"symbol", NULL, plugin_symbol, NULL, NULL, NULL, napi_default, NULL},
      {"registrations", NULL, pi_native_registrations, NULL, NULL, NULL,
       napi_default, NULL},
      {"invoke", NULL, pi_native_invoke, NULL, NULL, NULL, napi_default, NULL},
      {"close", NULL, plugin_close, NULL, NULL, NULL, napi_default, NULL},
  };
  if (napi_define_properties(env, result,
                             sizeof(methods) / sizeof(methods[0]), methods) != napi_ok ||
      napi_wrap(env, result, plugin, finalize_plugin, NULL, NULL) != napi_ok) {
    destroy_plugin(plugin);
    return throw_message(env, "could not initialize TinyCC plugin object");
  }
  return result;
}

int32_t NODE_API_MODULE_GET_API_VERSION(void) {
  return NAPI_VERSION;
}

napi_value NAPI_MODULE_INITIALIZER(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
      {"compile", NULL, compile_plugin, NULL, NULL, NULL, napi_default, NULL},
      {"typedCall", NULL, pi_typed_call, NULL, NULL, NULL, napi_default, NULL},
      {"pointer", NULL, pi_typed_pointer, NULL, NULL, NULL, napi_default, NULL},
      {"readPointer", NULL, pi_typed_read, NULL, NULL, NULL, napi_default, NULL},
      {"cstring", NULL, pi_typed_cstring, NULL, NULL, NULL, napi_default, NULL},
      {"toArrayBuffer", NULL, pi_typed_to_array_buffer, NULL, NULL, NULL,
       napi_default, NULL},
  };
  if (napi_define_properties(env, exports,
                             sizeof(properties) / sizeof(properties[0]),
                             properties) != napi_ok) {
    return NULL;
  }
  return exports;
}
