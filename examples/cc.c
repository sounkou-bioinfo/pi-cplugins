#include <stdarg.h>
#include <stdint.h>
#include <node_api.h>

#ifndef PI_CC_FACTOR
#define PI_CC_FACTOR 1
#endif

int32_t configured_answer(int32_t value) {
  return value * PI_CC_FACTOR;
}

int32_t call_transform(int32_t value, int32_t (*callback)(int32_t)) {
  return callback(value);
}

int32_t has_napi_env(napi_env env) {
  return env != 0;
}

napi_value identity_napi_value(napi_value value) {
  return value;
}

napi_value make_napi_answer(napi_env env) {
  napi_value result;
  if (napi_create_int32(env, 42, &result) != napi_ok) return 0;
  return result;
}

double sum_promoted_f32(int32_t count, ...) {
  double sum = 0.0;
  va_list arguments;
  va_start(arguments, count);
  for (int32_t i = 0; i < count; ++i) sum += va_arg(arguments, double);
  va_end(arguments);
  return sum;
}

int32_t sum_promoted_small(int32_t count, ...) {
  int32_t sum = 0;
  va_list arguments;
  va_start(arguments, count);
  for (int32_t i = 0; i < count; ++i) sum += va_arg(arguments, int);
  va_end(arguments);
  return sum;
}

int32_t sum_variadic(int32_t count, ...) {
  int32_t sum = 0;
  va_list arguments;
  va_start(arguments, count);
  for (int32_t i = 0; i < count; ++i) sum += va_arg(arguments, int32_t);
  va_end(arguments);
  return sum;
}
