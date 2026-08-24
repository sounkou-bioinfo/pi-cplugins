#include "pi_typed_node.h"

#include "pi_typed_call.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PI_TYPED_MAX_ARGS 64u
#define PI_TYPED_MAX_STRING_BYTES (16u * 1024u * 1024u)
#define PI_TYPED_MAX_READ_BYTES (64u * 1024u * 1024u)
#define PI_TYPED_MAX_SAFE_POINTER 9007199254740991.0

typedef enum pi_value_kind {
  PI_VALUE_VOID,
  PI_VALUE_BUFFER,
  PI_VALUE_BUFFER_LENGTH,
  PI_VALUE_CSTRING,
  PI_VALUE_FUNCTION,
  PI_VALUE_POINTER,
  PI_VALUE_I8,
  PI_VALUE_I16,
  PI_VALUE_I32,
  PI_VALUE_I64,
  PI_VALUE_I64_FAST,
  PI_VALUE_U8,
  PI_VALUE_U16,
  PI_VALUE_U32,
  PI_VALUE_U64,
  PI_VALUE_U64_FAST,
  PI_VALUE_F32,
  PI_VALUE_F64,
  PI_VALUE_BOOL,
  PI_VALUE_CHAR,
  PI_VALUE_NAPI_ENV,
  PI_VALUE_NAPI_VALUE,
  PI_VALUE_COMPOSITE
} pi_value_kind;

typedef void (*pi_memory_deallocator)(void* bytes, void* context);

typedef struct pi_external_memory {
  void* bytes;
  void* context;
  pi_memory_deallocator deallocator;
} pi_external_memory;

static napi_value typed_throw(napi_env env, const char* message) {
  napi_throw_type_error(env, NULL, message);
  return NULL;
}

static int value_is_null(napi_env env, napi_value value) {
  napi_value null_value;
  bool equal = false;
  return napi_get_null(env, &null_value) == napi_ok &&
         napi_strict_equals(env, value, null_value, &equal) == napi_ok && equal;
}

static int parse_kind_text(const char* text, pi_value_kind* kind) {
  if (strcmp(text, "void") == 0) *kind = PI_VALUE_VOID;
  else if (strcmp(text, "buffer") == 0) *kind = PI_VALUE_BUFFER;
  else if (strcmp(text, "buffer_length") == 0) *kind = PI_VALUE_BUFFER_LENGTH;
  else if (strcmp(text, "cstring") == 0) *kind = PI_VALUE_CSTRING;
  else if (strcmp(text, "function") == 0 || strcmp(text, "fn") == 0 ||
           strcmp(text, "callback") == 0) *kind = PI_VALUE_FUNCTION;
  else if (strcmp(text, "ptr") == 0 || strcmp(text, "pointer") == 0 ||
           strcmp(text, "void*") == 0 || strcmp(text, "char*") == 0) *kind = PI_VALUE_POINTER;
  else if (strcmp(text, "i8") == 0 || strcmp(text, "int8_t") == 0) *kind = PI_VALUE_I8;
  else if (strcmp(text, "i16") == 0 || strcmp(text, "int16_t") == 0) *kind = PI_VALUE_I16;
  else if (strcmp(text, "i32") == 0 || strcmp(text, "int32_t") == 0 ||
           strcmp(text, "int") == 0) *kind = PI_VALUE_I32;
  else if (strcmp(text, "i64") == 0 || strcmp(text, "int64_t") == 0 ||
           strcmp(text, "isize") == 0) *kind = PI_VALUE_I64;
  else if (strcmp(text, "i64_fast") == 0) *kind = PI_VALUE_I64_FAST;
  else if (strcmp(text, "u8") == 0 || strcmp(text, "uint8_t") == 0) *kind = PI_VALUE_U8;
  else if (strcmp(text, "u16") == 0 || strcmp(text, "uint16_t") == 0) *kind = PI_VALUE_U16;
  else if (strcmp(text, "u32") == 0 || strcmp(text, "uint32_t") == 0) *kind = PI_VALUE_U32;
  else if (strcmp(text, "u64") == 0 || strcmp(text, "uint64_t") == 0 ||
           strcmp(text, "usize") == 0) *kind = PI_VALUE_U64;
  else if (strcmp(text, "u64_fast") == 0) *kind = PI_VALUE_U64_FAST;
  else if (strcmp(text, "f32") == 0 || strcmp(text, "float") == 0) *kind = PI_VALUE_F32;
  else if (strcmp(text, "f64") == 0 || strcmp(text, "double") == 0) *kind = PI_VALUE_F64;
  else if (strcmp(text, "bool") == 0) *kind = PI_VALUE_BOOL;
  else if (strcmp(text, "char") == 0) *kind = PI_VALUE_CHAR;
  else if (strcmp(text, "napi_env") == 0) *kind = PI_VALUE_NAPI_ENV;
  else if (strcmp(text, "napi_value") == 0) *kind = PI_VALUE_NAPI_VALUE;
  else if (strncmp(text, "struct:", 7u) == 0 ||
           strncmp(text, "union:", 6u) == 0) *kind = PI_VALUE_COMPOSITE;
  else if (strncmp(text, "enum:", 5u) == 0) *kind = PI_VALUE_I32;
  else return 0;
  return 1;
}

static int parse_kind(napi_env env, napi_value value, pi_value_kind* kind) {
  size_t size = 0u;
  if (napi_get_value_string_utf8(env, value, NULL, 0u, &size) != napi_ok ||
      size == 0u || size > 127u) {
    typed_throw(env, "value types must be supported type strings");
    return 0;
  }
  char text[128];
  if (napi_get_value_string_utf8(env, value, text, sizeof(text), &size) != napi_ok ||
      !parse_kind_text(text, kind)) {
    typed_throw(env, "unsupported value type");
    return 0;
  }
  return 1;
}

static int parse_kind_array(napi_env env, napi_value value,
                            pi_value_kind** kinds, size_t* count) {
  bool is_array = false;
  uint32_t length = 0u;
  if (napi_is_array(env, value, &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, value, &length) != napi_ok ||
      length > PI_TYPED_MAX_ARGS) {
    typed_throw(env, "argument types must be an array with at most 64 entries");
    return 0;
  }
  pi_value_kind* parsed = NULL;
  if (length > 0u) {
    parsed = (pi_value_kind*)calloc(length, sizeof(*parsed));
    if (parsed == NULL) {
      typed_throw(env, "could not allocate argument types");
      return 0;
    }
  }
  for (uint32_t i = 0u; i < length; ++i) {
    napi_value item;
    if (napi_get_element(env, value, i, &item) != napi_ok ||
        !parse_kind(env, item, &parsed[i]) || parsed[i] == PI_VALUE_VOID) {
      bool pending = false;
      napi_is_exception_pending(env, &pending);
      free(parsed);
      if (!pending) typed_throw(env, "void is not an argument type");
      return 0;
    }
  }
  *kinds = parsed;
  *count = length;
  return 1;
}

static int get_array_view_info(napi_env env, napi_value value, void** pointer,
                               size_t* byte_length) {
  bool matched = false;
  if (napi_is_buffer(env, value, &matched) == napi_ok && matched) {
    return napi_get_buffer_info(env, value, pointer, byte_length) == napi_ok;
  }
  if (napi_is_typedarray(env, value, &matched) == napi_ok && matched) {
    napi_typedarray_type type;
    size_t length;
    napi_value array_buffer;
    size_t offset;
    if (napi_get_typedarray_info(env, value, &type, &length, pointer,
                                 &array_buffer, &offset) != napi_ok) return 0;
    size_t element_size = 1u;
    switch (type) {
      case napi_int16_array:
      case napi_uint16_array: element_size = 2u; break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array: element_size = 4u; break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array: element_size = 8u; break;
      default: break;
    }
    *byte_length = length * element_size;
    return 1;
  }
  if (napi_is_dataview(env, value, &matched) == napi_ok && matched) {
    napi_value array_buffer;
    size_t offset;
    return napi_get_dataview_info(env, value, byte_length, pointer,
                                  &array_buffer, &offset) == napi_ok;
  }
  return 0;
}

static int get_array_view(napi_env env, napi_value value, void** pointer) {
  size_t byte_length;
  return get_array_view_info(env, value, pointer, &byte_length);
}

static int get_pointer_depth(napi_env env, napi_value value, int require_view,
                             void** pointer, unsigned int depth) {
  if (value_is_null(env, value)) {
    *pointer = NULL;
    return !require_view;
  }
  if (get_array_view(env, value, pointer)) return 1;
  if (require_view) return 0;
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) return 0;
  if (type == napi_number) {
    double number;
    if (napi_get_value_double(env, value, &number) != napi_ok ||
        !isfinite(number) || number < 0.0 ||
        number > PI_TYPED_MAX_SAFE_POINTER || floor(number) != number) return 0;
    *pointer = (void*)(uintptr_t)number;
    return 1;
  }
  if (type == napi_bigint) {
    uint64_t address;
    bool lossless;
    if (napi_get_value_bigint_uint64(env, value, &address, &lossless) != napi_ok ||
        !lossless) return 0;
    *pointer = (void*)(uintptr_t)address;
    return 1;
  }
  if (type == napi_object && depth < 2u) {
    napi_value property;
    bool has_property = false;
    if (napi_has_named_property(env, value, "ptr", &has_property) == napi_ok &&
        has_property && napi_get_named_property(env, value, "ptr", &property) == napi_ok) {
      return get_pointer_depth(env, property, 0, pointer, depth + 1u);
    }
  }
  return 0;
}

static int get_pointer(napi_env env, napi_value value, int require_view,
                       void** pointer) {
  return get_pointer_depth(env, value, require_view, pointer, 0u);
}

static int get_integer(napi_env env, napi_value value, double minimum,
                       double maximum, double* number) {
  return napi_get_value_double(env, value, number) == napi_ok &&
         isfinite(*number) && floor(*number) == *number &&
         *number >= minimum && *number <= maximum;
}

static int get_i64(napi_env env, napi_value value, int64_t* result) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) return 0;
  if (type == napi_bigint) {
    bool lossless;
    return napi_get_value_bigint_int64(env, value, result, &lossless) == napi_ok &&
           lossless;
  }
  double number;
  if (!get_integer(env, value, -PI_TYPED_MAX_SAFE_POINTER,
                   PI_TYPED_MAX_SAFE_POINTER, &number)) return 0;
  *result = (int64_t)number;
  return 1;
}

static int get_u64(napi_env env, napi_value value, uint64_t* result) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) return 0;
  if (type == napi_bigint) {
    bool lossless;
    return napi_get_value_bigint_uint64(env, value, result, &lossless) == napi_ok &&
           lossless;
  }
  double number;
  if (!get_integer(env, value, 0.0, PI_TYPED_MAX_SAFE_POINTER, &number)) return 0;
  *result = (uint64_t)number;
  return 1;
}

static int value_from_js(napi_env env, napi_value value, pi_value_kind kind,
                         pi_typed_value* result, char** owned_string) {
  double number;
  bool boolean;
  size_t size;
  switch (kind) {
    case PI_VALUE_BUFFER:
      return get_pointer(env, value, 1, &result->value.pointer);
    case PI_VALUE_BUFFER_LENGTH: {
      void* pointer;
      size_t byte_length;
      if (!get_array_view_info(env, value, &pointer, &byte_length)) return 0;
      result->value.u64 = (uint64_t)byte_length;
      return 1;
    }
    case PI_VALUE_CSTRING: {
      napi_valuetype type;
      if (napi_typeof(env, value, &type) != napi_ok) return 0;
      if (type != napi_string) return get_pointer(env, value, 0, &result->value.pointer);
      if (napi_get_value_string_utf8(env, value, NULL, 0u, &size) != napi_ok ||
          size > PI_TYPED_MAX_STRING_BYTES) return 0;
      char* string = (char*)malloc(size + 1u);
      if (string == NULL ||
          napi_get_value_string_utf8(env, value, string, size + 1u, &size) != napi_ok) {
        free(string);
        return 0;
      }
      result->value.pointer = string;
      *owned_string = string;
      return 1;
    }
    case PI_VALUE_FUNCTION: {
      napi_valuetype type;
      if (napi_typeof(env, value, &type) != napi_ok) return 0;
      if (type == napi_function) {
        result->value.pointer = (void*)value;
        result->byte_length = PI_TYPED_JS_CALLBACK;
        return 1;
      }
      return get_pointer(env, value, 0, &result->value.pointer) &&
             result->value.pointer != NULL;
    }
    case PI_VALUE_POINTER:
      return get_pointer(env, value, 0, &result->value.pointer);
    case PI_VALUE_I8:
    case PI_VALUE_CHAR:
      if (!get_integer(env, value, INT8_MIN, INT8_MAX, &number)) return 0;
      result->value.i8 = (int8_t)number;
      return 1;
    case PI_VALUE_I16:
      if (!get_integer(env, value, INT16_MIN, INT16_MAX, &number)) return 0;
      result->value.i16 = (int16_t)number;
      return 1;
    case PI_VALUE_I32:
      if (!get_integer(env, value, INT32_MIN, INT32_MAX, &number)) return 0;
      result->value.i32 = (int32_t)number;
      return 1;
    case PI_VALUE_I64:
    case PI_VALUE_I64_FAST:
      return get_i64(env, value, &result->value.i64);
    case PI_VALUE_U8:
      if (!get_integer(env, value, 0.0, UINT8_MAX, &number)) return 0;
      result->value.u8 = (uint8_t)number;
      return 1;
    case PI_VALUE_U16:
      if (!get_integer(env, value, 0.0, UINT16_MAX, &number)) return 0;
      result->value.u16 = (uint16_t)number;
      return 1;
    case PI_VALUE_U32:
      if (!get_integer(env, value, 0.0, UINT32_MAX, &number)) return 0;
      result->value.u32 = (uint32_t)number;
      return 1;
    case PI_VALUE_U64:
    case PI_VALUE_U64_FAST:
      return get_u64(env, value, &result->value.u64);
    case PI_VALUE_F32:
      if (napi_get_value_double(env, value, &number) != napi_ok) return 0;
      result->value.f32 = (float)number;
      return 1;
    case PI_VALUE_F64:
      return napi_get_value_double(env, value, &result->value.f64) == napi_ok;
    case PI_VALUE_BOOL:
      if (napi_get_value_bool(env, value, &boolean) != napi_ok) return 0;
      result->value.u8 = boolean ? 1u : 0u;
      return 1;
    case PI_VALUE_NAPI_ENV:
      result->value.pointer = env;
      return 1;
    case PI_VALUE_NAPI_VALUE:
      result->value.pointer = value;
      return 1;
    case PI_VALUE_COMPOSITE:
      return get_array_view_info(env, value, &result->value.pointer,
                                 &result->byte_length);
    case PI_VALUE_VOID:
      return 0;
  }
  return 0;
}

static napi_value pointer_to_js(napi_env env, void* pointer) {
  napi_value result;
  if (pointer == NULL) napi_get_null(env, &result);
  else napi_create_double(env, (double)(uintptr_t)pointer, &result);
  return result;
}

static napi_value value_to_js(napi_env env, pi_value_kind kind,
                              const pi_typed_value* value) {
  napi_value result;
  switch (kind) {
    case PI_VALUE_VOID:
      napi_get_undefined(env, &result);
      return result;
    case PI_VALUE_BUFFER:
    case PI_VALUE_FUNCTION:
    case PI_VALUE_POINTER:
    case PI_VALUE_NAPI_ENV:
      return pointer_to_js(env, value->value.pointer);
    case PI_VALUE_CSTRING: {
      if (value->value.pointer == NULL) {
        napi_get_null(env, &result);
        return result;
      }
      const char* string = (const char*)value->value.pointer;
      size_t size = 0u;
      while (size <= PI_TYPED_MAX_STRING_BYTES && string[size] != '\0') ++size;
      if (size > PI_TYPED_MAX_STRING_BYTES ||
          napi_create_string_utf8(env, string, size, &result) != napi_ok) {
        return typed_throw(env, "cstring result is invalid or exceeds 16 MiB");
      }
      return result;
    }
    case PI_VALUE_I8:
    case PI_VALUE_CHAR: napi_create_int32(env, value->value.i8, &result); return result;
    case PI_VALUE_I16: napi_create_int32(env, value->value.i16, &result); return result;
    case PI_VALUE_I32: napi_create_int32(env, value->value.i32, &result); return result;
    case PI_VALUE_I64: napi_create_bigint_int64(env, value->value.i64, &result); return result;
    case PI_VALUE_I64_FAST: napi_create_double(env, (double)value->value.i64, &result); return result;
    case PI_VALUE_U8: napi_create_uint32(env, value->value.u8, &result); return result;
    case PI_VALUE_U16: napi_create_uint32(env, value->value.u16, &result); return result;
    case PI_VALUE_U32: napi_create_uint32(env, value->value.u32, &result); return result;
    case PI_VALUE_U64: napi_create_bigint_uint64(env, value->value.u64, &result); return result;
    case PI_VALUE_U64_FAST: napi_create_double(env, (double)value->value.u64, &result); return result;
    case PI_VALUE_F32: napi_create_double(env, value->value.f32, &result); return result;
    case PI_VALUE_F64: napi_create_double(env, value->value.f64, &result); return result;
    case PI_VALUE_BOOL: napi_get_boolean(env, value->value.u8 != 0u, &result); return result;
    case PI_VALUE_NAPI_VALUE: return (napi_value)value->value.pointer;
    case PI_VALUE_COMPOSITE: {
      if (value->value.pointer == NULL || value->byte_length == 0u) {
        free(value->value.pointer);
        return typed_throw(env, "generated composite result is invalid");
      }
      const napi_status status = napi_create_buffer_copy(
        env, value->byte_length, value->value.pointer, NULL, &result);
      free(value->value.pointer);
      if (status != napi_ok) return typed_throw(env, "could not copy composite result");
      return result;
    }
    case PI_VALUE_BUFFER_LENGTH:
      return typed_throw(env, "buffer_length is argument-only");
  }
  return typed_throw(env, "unsupported result type");
}

napi_value pi_typed_call(napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4u;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 4u) {
    return typed_throw(env, "typedCall expects thunk, types, result type, and values");
  }
  void* thunk_pointer = NULL;
  if (!get_pointer(env, argv[0], 0, &thunk_pointer) || thunk_pointer == NULL ||
      sizeof(pi_typed_call_fn) != sizeof(thunk_pointer)) {
    return typed_throw(env, "generated thunk pointer is invalid");
  }
  pi_typed_call_fn thunk = NULL;
  memcpy(&thunk, &thunk_pointer, sizeof(thunk));

  pi_value_kind* kinds = NULL;
  size_t argument_count = 0u;
  if (!parse_kind_array(env, argv[1], &kinds, &argument_count)) return NULL;
  pi_value_kind result_kind;
  if (!parse_kind(env, argv[2], &result_kind) || result_kind == PI_VALUE_NAPI_ENV ||
      result_kind == PI_VALUE_BUFFER_LENGTH) {
    free(kinds);
    return typed_throw(env, "invalid result type");
  }

  bool values_are_array = false;
  uint32_t value_count = 0u;
  if (napi_is_array(env, argv[3], &values_are_array) != napi_ok || !values_are_array ||
      napi_get_array_length(env, argv[3], &value_count) != napi_ok) {
    free(kinds);
    return typed_throw(env, "typed call values must be an array");
  }
  size_t visible_count = 0u;
  for (size_t i = 0u; i < argument_count; ++i) {
    if (kinds[i] != PI_VALUE_NAPI_ENV) ++visible_count;
  }
  if (value_count != visible_count) {
    free(kinds);
    return typed_throw(env, "typed call value count does not match its signature");
  }

  pi_typed_value* arguments = NULL;
  char** strings = NULL;
  if (argument_count > 0u) {
    arguments = (pi_typed_value*)calloc(argument_count, sizeof(*arguments));
    strings = (char**)calloc(argument_count, sizeof(*strings));
  }
  if (argument_count > 0u && (arguments == NULL || strings == NULL)) {
    free(arguments);
    free(strings);
    free(kinds);
    return typed_throw(env, "could not allocate typed call arguments");
  }

  size_t visible_index = 0u;
  int converted = 1;
  for (size_t i = 0u; i < argument_count; ++i) {
    napi_value value = NULL;
    if (kinds[i] != PI_VALUE_NAPI_ENV &&
        napi_get_element(env, argv[3], (uint32_t)visible_index++, &value) != napi_ok) {
      converted = 0;
      break;
    }
    if (!value_from_js(env, value, kinds[i], &arguments[i], &strings[i])) {
      converted = 0;
      break;
    }
  }

  napi_value js_result = NULL;
  if (!converted) {
    js_result = typed_throw(env, "typed argument does not match its declared type");
  } else {
    pi_typed_value result;
    memset(&result, 0, sizeof(result));
    const int32_t status = thunk((void*)env, arguments, argument_count, &result);
    if (status != 0) {
      js_result = typed_throw(env, "generated TinyCC thunk rejected the call");
    } else {
      bool pending = false;
      if (napi_is_exception_pending(env, &pending) != napi_ok || pending) {
        if (result_kind == PI_VALUE_COMPOSITE) free(result.value.pointer);
        js_result = NULL;
      } else {
        js_result = value_to_js(env, result_kind, &result);
      }
    }
  }

  for (size_t i = 0u; i < argument_count; ++i) free(strings[i]);
  free(strings);
  free(arguments);
  free(kinds);
  return js_result;
}

napi_value pi_typed_pointer(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1u;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 1u) {
    return typed_throw(env, "pointer expects one ArrayBuffer view");
  }
  void* pointer;
  if (!get_array_view(env, argv[0], &pointer)) {
    return typed_throw(env, "pointer expects a Buffer, TypedArray, or DataView");
  }
  return pointer_to_js(env, pointer);
}

napi_value pi_typed_read(napi_env env, napi_callback_info info) {
  napi_value argv[3];
  size_t argc = 3u;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 2u) {
    return typed_throw(env, "readPointer expects pointer, byte length, and optional offset");
  }
  void* base;
  double length_number;
  double offset_number = 0.0;
  if (!get_pointer(env, argv[0], 0, &base) || base == NULL ||
      !get_integer(env, argv[1], 0.0, PI_TYPED_MAX_READ_BYTES, &length_number) ||
      (argc == 3u && !get_integer(env, argv[2], 0.0,
                                  PI_TYPED_MAX_SAFE_POINTER, &offset_number))) {
    return typed_throw(env, "readPointer argument is invalid");
  }
  napi_value result;
  if (napi_create_buffer_copy(env, (size_t)length_number,
                              (const uint8_t*)base + (size_t)offset_number,
                              NULL, &result) != napi_ok) {
    return typed_throw(env, "could not copy pointer bytes");
  }
  return result;
}

napi_value pi_typed_cstring(napi_env env, napi_callback_info info) {
  napi_value argv[3];
  size_t argc = 3u;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1u) {
    return typed_throw(env, "cstring expects pointer and optional offset/length");
  }
  void* base;
  double offset_number = 0.0;
  if (!get_pointer(env, argv[0], 0, &base) || base == NULL ||
      (argc >= 2u && !get_integer(env, argv[1], 0.0,
                                  PI_TYPED_MAX_SAFE_POINTER, &offset_number))) {
    return typed_throw(env, "cstring pointer or offset is invalid");
  }
  const char* string = (const char*)base + (size_t)offset_number;
  size_t length = 0u;
  if (argc == 3u) {
    double length_number;
    if (!get_integer(env, argv[2], 0.0, PI_TYPED_MAX_STRING_BYTES,
                     &length_number)) return typed_throw(env, "cstring length is invalid");
    length = (size_t)length_number;
  } else {
    while (length <= PI_TYPED_MAX_STRING_BYTES && string[length] != '\0') ++length;
    if (length > PI_TYPED_MAX_STRING_BYTES) {
      return typed_throw(env, "cstring exceeds 16 MiB without a terminator");
    }
  }
  napi_value result;
  if (napi_create_string_utf8(env, string, length, &result) != napi_ok) {
    return typed_throw(env, "could not copy cstring");
  }
  return result;
}

static void finalize_external_memory(napi_env env, void* data, void* hint) {
  (void)env;
  (void)data;
  pi_external_memory* memory = (pi_external_memory*)hint;
  if (memory != NULL) {
    memory->deallocator(memory->bytes, memory->context);
    free(memory);
  }
}

napi_value pi_typed_to_array_buffer(napi_env env, napi_callback_info info) {
  napi_value argv[5];
  size_t argc = 5u;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 5u) {
    return typed_throw(env, "toArrayBuffer expects pointer, offset, length, context, and deallocator");
  }
  void* base;
  double offset_number;
  if (!get_pointer(env, argv[0], 0, &base) || base == NULL ||
      !get_integer(env, argv[1], 0.0, PI_TYPED_MAX_SAFE_POINTER,
                   &offset_number)) {
    return typed_throw(env, "toArrayBuffer pointer or offset is invalid");
  }
  uint8_t* bytes = (uint8_t*)base + (size_t)offset_number;
  size_t length = 0u;
  if (value_is_null(env, argv[2])) {
    while (length <= PI_TYPED_MAX_READ_BYTES && bytes[length] != 0u) ++length;
    if (length > PI_TYPED_MAX_READ_BYTES) {
      return typed_throw(env, "toArrayBuffer exceeds 64 MiB without a terminator");
    }
  } else {
    double length_number;
    if (!get_integer(env, argv[2], 0.0, PI_TYPED_MAX_READ_BYTES,
                     &length_number)) return typed_throw(env, "toArrayBuffer length is invalid");
    length = (size_t)length_number;
  }

  pi_external_memory* memory = NULL;
  napi_finalize finalizer = NULL;
  if (!value_is_null(env, argv[4])) {
    void* deallocator_pointer;
    void* context = NULL;
    if (!get_pointer(env, argv[4], 0, &deallocator_pointer) ||
        deallocator_pointer == NULL ||
        (!value_is_null(env, argv[3]) &&
         !get_pointer(env, argv[3], 0, &context))) {
      return typed_throw(env, "toArrayBuffer deallocator or context is invalid");
    }
    memory = (pi_external_memory*)calloc(1u, sizeof(*memory));
    if (memory == NULL || sizeof(memory->deallocator) != sizeof(deallocator_pointer)) {
      free(memory);
      return typed_throw(env, "could not allocate toArrayBuffer deallocator");
    }
    memory->bytes = bytes;
    memory->context = context;
    memcpy(&memory->deallocator, &deallocator_pointer, sizeof(memory->deallocator));
    finalizer = finalize_external_memory;
  }
  napi_value result;
  if (napi_create_external_arraybuffer(env, bytes, length, finalizer, memory,
                                       &result) != napi_ok) {
    free(memory);
    return typed_throw(env, "Node runtime rejected the external ArrayBuffer");
  }
  return result;
}
