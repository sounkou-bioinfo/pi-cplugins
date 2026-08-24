#ifndef PI_NATIVE_PLUGIN_H
#define PI_NATIVE_PLUGIN_H

#include <node_api.h>

#include "libtcc.h"
#include "pi_plugin.h"

#define PI_NATIVE_MAX_REGISTRATIONS 128u

typedef enum pi_native_registration_kind {
  PI_NATIVE_TOOL,
  PI_NATIVE_COMMAND,
  PI_NATIVE_EVENT
} pi_native_registration_kind;

typedef struct pi_native_registration {
  pi_native_registration_kind kind;
  const char* name;
  const char* label;
  const char* description;
  const char* parameters_json;
  pi_plugin_callback_fn execute;
  void* callback_data;
} pi_native_registration;

typedef struct compile_errors {
  char text[4096u];
  size_t length;
} compile_errors;

typedef struct compiled_plugin {
  TCCState* compiler;
  pi_plugin_api* api;
  compile_errors errors;
  pi_native_registration registrations[PI_NATIVE_MAX_REGISTRATIONS];
  size_t registration_count;
} compiled_plugin;

void pi_native_host_api(compiled_plugin* plugin, pi_plugin_host_api* host);
napi_value pi_native_registrations(napi_env env, napi_callback_info info);
napi_value pi_native_invoke(napi_env env, napi_callback_info info);

#endif
