#ifndef PI_TYPED_NODE_H
#define PI_TYPED_NODE_H

#include <node_api.h>

napi_value pi_typed_call(napi_env env, napi_callback_info info);
napi_value pi_typed_pointer(napi_env env, napi_callback_info info);
napi_value pi_typed_read(napi_env env, napi_callback_info info);
napi_value pi_typed_cstring(napi_env env, napi_callback_info info);
napi_value pi_typed_to_array_buffer(napi_env env, napi_callback_info info);

#endif
