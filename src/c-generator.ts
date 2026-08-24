import type {
  BindingFunction,
  BindingKind,
  BindingParameter,
  CallbackParameter,
  CallbackSignature,
} from "./c-parser.js";

export interface GeneratedBindings {
  source: string;
  manifest: {
    name: string;
    callSurface: "generated-bindings";
    functions: BindingFunction[];
  };
}

function cString(value: string): string {
  return JSON.stringify(value).replace(/\u2028|\u2029/g, " ");
}

function isComposite(kind: BindingKind): boolean {
  return kind.startsWith("struct:") || kind.startsWith("union:");
}

function isEnum(kind: BindingKind): boolean {
  return kind.startsWith("enum:");
}

function canonicalType(kind: Exclude<BindingKind, "void">): string {
  if (isComposite(kind) || isEnum(kind)) return kind.replace(":", " ");
  switch (kind) {
    case "bool": return "_Bool";
    case "char":
    case "i8": return "int8_t";
    case "i16": return "int16_t";
    case "i32": return "int32_t";
    case "i64":
    case "i64_fast": return "int64_t";
    case "u8": return "uint8_t";
    case "u16": return "uint16_t";
    case "u32": return "uint32_t";
    case "u64":
    case "u64_fast":
    case "buffer_length": return "uint64_t";
    case "f32": return "float";
    case "f64": return "double";
    case "cstring": return "const char*";
    case "buffer": return "char*";
    case "function":
    case "ptr":
    case "napi_env":
    case "napi_value": return "void*";
  }
  throw new Error(`unsupported generated C type '${kind}'`);
}

function readFunction(kind: Exclude<BindingKind, "void">): string {
  return kind === "cstring" ? "pi_cplugins_read_string" : `pi_cplugins_read_${kind}`;
}

function fixedOutputSize(kind: BindingKind): number | undefined {
  switch (kind) {
    case "void":
      return 0;
    case "bool":
      return 1;
    case "i32":
    case "u32":
    case "f32":
      return 4;
    case "i64":
    case "u64":
    case "f64":
      return 8;
    case "cstring":
      return undefined;
    default:
      return undefined;
  }
}

function writeResult(kind: BindingKind): string {
  switch (kind) {
    case "bool":
      return "output[0] = pi_result ? 1u : 0u;";
    case "i32":
      return "pi_cplugins_write_u32(output, (uint32_t)pi_result);";
    case "u32":
      return "pi_cplugins_write_u32(output, pi_result);";
    case "i64":
      return "pi_cplugins_write_u64(output, (uint64_t)pi_result);";
    case "u64":
      return "pi_cplugins_write_u64(output, pi_result);";
    case "f32":
      return "pi_cplugins_write_f32(output, pi_result);";
    case "f64":
      return "pi_cplugins_write_f64(output, pi_result);";
    default:
      throw new Error(`FFI type '${kind}' has no bounded-byte result codec`);
  }
}

function typedMember(kind: BindingKind): string {
  if (isComposite(kind)) return "pointer";
  if (isEnum(kind)) return "i32";
  switch (kind) {
    case "bool":
    case "u8": return "u8";
    case "char":
    case "i8": return "i8";
    case "i16": return "i16";
    case "i32": return "i32";
    case "i64":
    case "i64_fast": return "i64";
    case "u16": return "u16";
    case "u32": return "u32";
    case "u64":
    case "u64_fast":
    case "buffer_length": return "u64";
    case "f32": return "f32";
    case "f64": return "f64";
    case "buffer":
    case "cstring":
    case "function":
    case "ptr":
    case "napi_env":
    case "napi_value": return "pointer";
    case "void": throw new Error("void is not an argument type");
  }
  throw new Error(`unsupported generated value type '${kind}'`);
}

function callbackCParameters(parameters: CallbackParameter[]): string {
  return parameters.length === 0
    ? "void"
    : parameters.map((parameter, index) => `${parameter.cType} argument_${index}`).join(", ");
}

function callbackDefault(signature: CallbackSignature): string {
  if (signature.returnKind === "void") return "return;";
  if (isComposite(signature.returnKind)) {
    return `${signature.returnCType} empty; memset(&empty, 0, sizeof(empty)); return empty;`;
  }
  if (signature.returnKind === "ptr" || signature.returnKind === "buffer") {
    return "return NULL;";
  }
  return `return (${signature.returnCType})0;`;
}

function callbackArgumentToJs(parameter: CallbackParameter, index: number,
                              failure: string): string {
  const value = `argument_${index}`;
  const target = `js_arguments[${index}]`;
  if (isComposite(parameter.kind)) {
    return `if (napi_create_buffer_copy(context->env, sizeof(${value}), &${value}, NULL, &${target}) != napi_ok) { ${failure} }`;
  }
  if (isEnum(parameter.kind) || parameter.kind === "i8" || parameter.kind === "i16" ||
      parameter.kind === "i32" || parameter.kind === "char") {
    return `if (napi_create_int32(context->env, (int32_t)${value}, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "u8" || parameter.kind === "u16" || parameter.kind === "u32") {
    return `if (napi_create_uint32(context->env, (uint32_t)${value}, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "i64" || parameter.kind === "i64_fast") {
    return `if (napi_create_bigint_int64(context->env, (int64_t)${value}, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "u64" || parameter.kind === "u64_fast" ||
      parameter.kind === "buffer_length") {
    return `if (napi_create_bigint_uint64(context->env, (uint64_t)${value}, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "f32" || parameter.kind === "f64") {
    return `if (napi_create_double(context->env, (double)${value}, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "bool") {
    return `if (napi_get_boolean(context->env, ${value} != 0, &${target}) != napi_ok) { ${failure} }`;
  }
  if (parameter.kind === "cstring") {
    return `if (${value} == NULL) {
    if (napi_get_null(context->env, &${target}) != napi_ok) { ${failure} }
  } else if (napi_create_string_utf8(context->env, ${value}, NAPI_AUTO_LENGTH, &${target}) != napi_ok) { ${failure} }`;
  }
  return `if (${value} == NULL) {
    if (napi_get_null(context->env, &${target}) != napi_ok) { ${failure} }
  } else if (napi_create_double(context->env, (double)(uintptr_t)${value}, &${target}) != napi_ok) { ${failure} }`;
}

function callbackResultFromJs(signature: CallbackSignature, failure: string): string {
  const kind = signature.returnKind;
  if (kind === "void") return "return;";
  if (isComposite(kind)) {
    return `void* bytes = NULL;
  size_t byte_length = 0u;
  bool is_buffer = false;
  ${signature.returnCType} value;
  if (napi_is_buffer(context->env, js_result, &is_buffer) != napi_ok || !is_buffer ||
      napi_get_buffer_info(context->env, js_result, &bytes, &byte_length) != napi_ok ||
      byte_length < sizeof(value)) { ${failure} }
  memcpy(&value, bytes, sizeof(value));
  return value;`;
  }
  if (isEnum(kind) || kind === "i8" || kind === "i16" || kind === "i32" || kind === "char") {
    return `int32_t value; if (napi_get_value_int32(context->env, js_result, &value) != napi_ok) { ${failure} } return (${signature.returnCType})value;`;
  }
  if (kind === "u8" || kind === "u16" || kind === "u32") {
    return `uint32_t value; if (napi_get_value_uint32(context->env, js_result, &value) != napi_ok) { ${failure} } return (${signature.returnCType})value;`;
  }
  if (kind === "i64" || kind === "i64_fast") {
    return `int64_t value; bool lossless; if (napi_get_value_bigint_int64(context->env, js_result, &value, &lossless) != napi_ok || !lossless) { ${failure} } return (${signature.returnCType})value;`;
  }
  if (kind === "u64" || kind === "u64_fast" || kind === "buffer_length") {
    return `uint64_t value; bool lossless; if (napi_get_value_bigint_uint64(context->env, js_result, &value, &lossless) != napi_ok || !lossless) { ${failure} } return (${signature.returnCType})value;`;
  }
  if (kind === "f32" || kind === "f64") {
    return `double value; if (napi_get_value_double(context->env, js_result, &value) != napi_ok) { ${failure} } return (${signature.returnCType})value;`;
  }
  if (kind === "bool") {
    return `bool value; if (napi_get_value_bool(context->env, js_result, &value) != napi_ok) { ${failure} } return (${signature.returnCType})(value ? 1 : 0);`;
  }
  return `double value; if (napi_get_value_double(context->env, js_result, &value) != napi_ok || value < 0.0) { ${failure} } return (${signature.returnCType})(uintptr_t)value;`;
}

function generateCallback(fn: BindingFunction, parameter: BindingParameter,
                          index: number): string {
  const signature = parameter.callback;
  if (!signature) return "";
  const prefix = `pi_cplugins_callback_${fn.id}_${index}`;
  const failure = callbackDefault(signature);
  const conversions = signature.parameters
    .map((item, argumentIndex) => callbackArgumentToJs(item, argumentIndex, failure))
    .join("\n  ");
  return `typedef struct ${prefix}_context {
  napi_env env;
  napi_value function;
  struct ${prefix}_context* previous;
} ${prefix}_context;

static ${prefix}_context* ${prefix}_current = NULL;

static ${signature.returnCType} ${prefix}(${callbackCParameters(signature.parameters)}) {
  ${prefix}_context* context = ${prefix}_current;
  if (context == NULL) { ${failure} }
  napi_value js_arguments[${Math.max(1, signature.parameters.length)}];
  napi_value receiver;
  napi_value js_result;
  ${conversions}
  if (napi_get_undefined(context->env, &receiver) != napi_ok ||
      napi_call_function(context->env, receiver, context->function,
                         ${signature.parameters.length}u, js_arguments, &js_result) != napi_ok) {
    ${failure}
  }
  ${callbackResultFromJs(signature, failure)}
}`;
}

function typedArgument(parameter: BindingParameter, index: number): string {
  const value = `arguments[${index}].value.${typedMember(parameter.kind)}`;
  if (parameter.callback) throw new Error("callback arguments are generated separately");
  if (isComposite(parameter.kind)) return `pi_arg_${index}`;
  if (isEnum(parameter.kind)) return `(${canonicalType(parameter.kind)})${value}`;
  switch (parameter.kind) {
    case "cstring": return `(const char*)${value}`;
    case "buffer": return `(char*)${value}`;
    default: return value;
  }
}

function generateTypedWrapper(fn: BindingFunction): string {
  const callbacks = fn.parameters
    .map((parameter, index) => generateCallback(fn, parameter, index))
    .filter(Boolean)
    .join("\n\n");
  const declarations = fn.parameters.flatMap((parameter, index) => {
    const lines: string[] = [];
    if (isComposite(parameter.kind)) {
      lines.push(`${canonicalType(parameter.kind)} pi_arg_${index};`);
      lines.push(`if (arguments[${index}].value.pointer == NULL || arguments[${index}].byte_length < sizeof(pi_arg_${index})) return PI_PLUGIN_ERR_INVALID_ARGUMENT;`);
      lines.push(`memcpy(&pi_arg_${index}, arguments[${index}].value.pointer, sizeof(pi_arg_${index}));`);
    }
    if (parameter.callback) {
      const prefix = `pi_cplugins_callback_${fn.id}_${index}`;
      lines.push(`${prefix}_context pi_context_${index} = { (napi_env)host_context, (napi_value)arguments[${index}].value.pointer, ${prefix}_current };`);
      lines.push(`if (arguments[${index}].byte_length == PI_TYPED_JS_CALLBACK) ${prefix}_current = &pi_context_${index};`);
    }
    return lines;
  }).join("\n  ");
  const args = fn.parameters.map((parameter, index) => {
    if (parameter.callback) {
      const signature = parameter.callback;
      const pointerType = `${signature.returnCType} (*)(${signature.parameters.length === 0
        ? "void"
        : signature.parameters.map((item) => item.cType).join(", ")})`;
      return `(arguments[${index}].byte_length == PI_TYPED_JS_CALLBACK ? pi_cplugins_callback_${fn.id}_${index} : (${pointerType})arguments[${index}].value.pointer)`;
    }
    return typedArgument(parameter, index);
  }).join(", ");
  const restores = fn.parameters.flatMap((parameter, index) => parameter.callback
    ? [`if (arguments[${index}].byte_length == PI_TYPED_JS_CALLBACK) pi_cplugins_callback_${fn.id}_${index}_current = pi_context_${index}.previous;`]
    : []).join("\n  ");
  const call = `${fn.name}(${args})`;
  let body: string;
  if (fn.returnKind === "void") {
    body = `${call};\n  ${restores}`;
  } else if (isComposite(fn.returnKind)) {
    body = `${canonicalType(fn.returnKind)} pi_result = ${call};
  ${restores}
  result->value.pointer = malloc(sizeof(pi_result));
  if (result->value.pointer == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  memcpy(result->value.pointer, &pi_result, sizeof(pi_result));
  result->byte_length = sizeof(pi_result);`;
  } else if (
    fn.returnKind === "buffer" || fn.returnKind === "cstring" ||
    fn.returnKind === "function" || fn.returnKind === "ptr" ||
    fn.returnKind === "napi_value"
  ) {
    body = `${canonicalType(fn.returnKind)} pi_result = ${call};\n  ${restores}\n  result->value.pointer = (void*)pi_result;`;
  } else {
    body = `${canonicalType(fn.returnKind)} pi_result = ${call};\n  ${restores}\n  result->value.${typedMember(fn.returnKind)} = pi_result;`;
  }
  return `${callbacks ? `${callbacks}\n\n` : ""}int32_t pi_cplugins_typed_${fn.id}(void* host_context,
                                      const pi_typed_value* arguments,
                                      size_t argument_count,
                                      pi_typed_value* result) {
  if (argument_count != ${fn.parameters.length}u || result == NULL ||
      (${fn.parameters.length}u > 0u && arguments == NULL)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
  ${declarations}
  ${body}
  return PI_PLUGIN_OK;
}`;
}

function compositeKinds(functions: BindingFunction[]): BindingKind[] {
  const kinds = new Set<BindingKind>();
  for (const fn of functions) {
    if (isComposite(fn.returnKind)) kinds.add(fn.returnKind);
    for (const parameter of fn.parameters) {
      if (isComposite(parameter.kind)) kinds.add(parameter.kind);
      if (parameter.callback) {
        if (isComposite(parameter.callback.returnKind)) kinds.add(parameter.callback.returnKind);
        for (const item of parameter.callback.parameters) {
          if (isComposite(item.kind)) kinds.add(item.kind);
        }
      }
    }
  }
  return [...kinds];
}

function generateCompositeLayouts(functions: BindingFunction[]): string {
  return compositeKinds(functions).map((kind) => {
    const suffix = kind.replace(":", "_");
    const cType = canonicalType(kind as Exclude<BindingKind, "void">);
    return `int32_t pi_cplugins_sizeof_${suffix}(void* host_context,
                                    const pi_typed_value* arguments,
                                    size_t argument_count,
                                    pi_typed_value* result) {
  (void)host_context;
  (void)arguments;
  if (argument_count != 0u || result == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  result->value.u64 = (uint64_t)sizeof(${cType});
  return PI_PLUGIN_OK;
}

int32_t pi_cplugins_alignof_${suffix}(void* host_context,
                                     const pi_typed_value* arguments,
                                     size_t argument_count,
                                     pi_typed_value* result) {
  (void)host_context;
  (void)arguments;
  if (argument_count != 0u || result == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  result->value.u64 = (uint64_t)_Alignof(${cType});
  return PI_PLUGIN_OK;
}`;
  }).join("\n\n");
}

function generateCase(fn: BindingFunction): string {
  const wireKinds = new Set<BindingKind>([
    "bool", "i32", "u32", "i64", "u64", "f32", "f64", "cstring", "void",
  ]);
  if (!wireKinds.has(fn.returnKind) || fn.parameters.some((parameter) => !wireKinds.has(parameter.kind))) {
    return `    case ${fn.id}u:\n      return PI_PLUGIN_ERR_INVALID_ARGUMENT;`;
  }

  const declarations = fn.parameters
    .map((parameter, index) => `      ${canonicalType(parameter.kind)} pi_arg_${index};`)
    .join("\n");
  const reads = fn.parameters
    .map(
      (parameter, index) =>
        `      if (!${readFunction(parameter.kind)}(&pi_cursor, pi_end, &pi_arg_${index})) return PI_PLUGIN_ERR_INVALID_ARGUMENT;`,
    )
    .join("\n");
  const args = fn.parameters.map((_, index) => `pi_arg_${index}`).join(", ");
  const call = `${fn.name}(${args})`;

  let result: string;
  if (fn.returnKind === "void") {
    result = `      ${call};
      *output_size = 0u;
      return PI_PLUGIN_OK;`;
  } else if (fn.returnKind === "cstring") {
    result = `      const char* pi_result = ${call};
      if (pi_result == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
      size_t pi_required = 0u;
      while (pi_required <= api->max_payload_bytes && pi_result[pi_required] != '\\0') ++pi_required;
      if (pi_required > api->max_payload_bytes) return PI_PLUGIN_ERR_PAYLOAD_TOO_LARGE;
      *output_size = pi_required;
      if (output_capacity < pi_required) return PI_PLUGIN_ERR_BUFFER_TOO_SMALL;
      if (pi_required > 0u && output == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
      memcpy(output, pi_result, pi_required);
      return PI_PLUGIN_OK;`;
  } else {
    const size = fixedOutputSize(fn.returnKind);
    result = `      ${canonicalType(fn.returnKind)} pi_result = (${canonicalType(fn.returnKind)})${call};
      *output_size = ${size}u;
      if (output_capacity < ${size}u) return PI_PLUGIN_ERR_BUFFER_TOO_SMALL;
      if (output == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
      ${writeResult(fn.returnKind)}
      return PI_PLUGIN_OK;`;
  }

  return `    case ${fn.id}u: {
${declarations ? `${declarations}\n` : ""}${reads ? `${reads}\n` : ""}      if (pi_cursor != pi_end) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
${result}
    }`;
}

const bridgeHelpers = String.raw`
#include "pi_plugin.h"
#include "pi_typed_call.h"
#include <node_api.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PI_CPLUGINS_GENERATED_MAX_PAYLOAD (64u * 1024u)

typedef struct pi_cplugins_generated_api {
  pi_plugin_api base;
  pi_plugin_host_api host;
} pi_cplugins_generated_api;

static int pi_cplugins_take(const uint8_t** cursor, const uint8_t* end,
                            size_t count, const uint8_t** bytes) {
  if ((size_t)(end - *cursor) < count) return 0;
  *bytes = *cursor;
  *cursor += count;
  return 1;
}

static int pi_cplugins_read_u32(const uint8_t** cursor, const uint8_t* end,
                                uint32_t* value) {
  const uint8_t* bytes;
  if (!pi_cplugins_take(cursor, end, 4u, &bytes)) return 0;
  *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
  return 1;
}

static int pi_cplugins_read_i32(const uint8_t** cursor, const uint8_t* end,
                                int32_t* value) {
  uint32_t bits;
  if (!pi_cplugins_read_u32(cursor, end, &bits)) return 0;
  memcpy(value, &bits, sizeof(bits));
  return 1;
}

static int pi_cplugins_read_u64(const uint8_t** cursor, const uint8_t* end,
                                uint64_t* value) {
  const uint8_t* bytes;
  if (!pi_cplugins_take(cursor, end, 8u, &bytes)) return 0;
  *value = (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8u) |
           ((uint64_t)bytes[2] << 16u) | ((uint64_t)bytes[3] << 24u) |
           ((uint64_t)bytes[4] << 32u) | ((uint64_t)bytes[5] << 40u) |
           ((uint64_t)bytes[6] << 48u) | ((uint64_t)bytes[7] << 56u);
  return 1;
}

static int pi_cplugins_read_i64(const uint8_t** cursor, const uint8_t* end,
                                int64_t* value) {
  uint64_t bits;
  if (!pi_cplugins_read_u64(cursor, end, &bits)) return 0;
  memcpy(value, &bits, sizeof(bits));
  return 1;
}

static int pi_cplugins_read_bool(const uint8_t** cursor, const uint8_t* end,
                                 _Bool* value) {
  const uint8_t* bytes;
  if (!pi_cplugins_take(cursor, end, 1u, &bytes) || bytes[0] > 1u) return 0;
  *value = bytes[0] != 0u;
  return 1;
}

static int pi_cplugins_read_f32(const uint8_t** cursor, const uint8_t* end,
                                float* value) {
  uint32_t bits;
  if (!pi_cplugins_read_u32(cursor, end, &bits)) return 0;
  memcpy(value, &bits, sizeof(bits));
  return 1;
}

static int pi_cplugins_read_f64(const uint8_t** cursor, const uint8_t* end,
                                double* value) {
  uint64_t bits;
  if (!pi_cplugins_read_u64(cursor, end, &bits)) return 0;
  memcpy(value, &bits, sizeof(bits));
  return 1;
}

static int pi_cplugins_read_string(const uint8_t** cursor, const uint8_t* end,
                                   const char** value) {
  uint32_t size;
  const uint8_t* bytes;
  if (!pi_cplugins_read_u32(cursor, end, &size) || size == UINT32_MAX ||
      !pi_cplugins_take(cursor, end, (size_t)size + 1u, &bytes) ||
      bytes[size] != 0u || memchr(bytes, 0, size) != NULL) return 0;
  *value = (const char*)bytes;
  return 1;
}

static void pi_cplugins_write_u32(uint8_t* output, uint32_t value) {
  output[0] = (uint8_t)value;
  output[1] = (uint8_t)(value >> 8u);
  output[2] = (uint8_t)(value >> 16u);
  output[3] = (uint8_t)(value >> 24u);
}

static void pi_cplugins_write_u64(uint8_t* output, uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i) output[i] = (uint8_t)(value >> (i * 8u));
}

static void pi_cplugins_write_f32(uint8_t* output, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  pi_cplugins_write_u32(output, bits);
}

static void pi_cplugins_write_f64(uint8_t* output, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  pi_cplugins_write_u64(output, bits);
}
`;

export function generatePluginSource(
  source: string,
  functions: BindingFunction[],
  sourceName: string,
): GeneratedBindings {
  const manifest = {
    name: sourceName,
    callSurface: "generated-bindings" as const,
    functions,
  };
  const manifestJson = JSON.stringify(manifest);
  const cases = functions.map(generateCase).join("\n");
  const typedWrappers = functions.map(generateTypedWrapper).join("\n\n");
  const compositeLayouts = generateCompositeLayouts(functions);
  const escapedSourceName = sourceName
    .replace(/[\r\n]/g, "_")
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"');

  const adapter = `${bridgeHelpers}
${typedWrappers}

${compositeLayouts}

static const char pi_cplugins_manifest_json[] = ${cString(manifestJson)};

static const char* pi_cplugins_generated_manifest(const pi_plugin_api* api,
                                                  size_t* byte_count) {
  (void)api;
  if (byte_count != NULL) *byte_count = sizeof(pi_cplugins_manifest_json) - 1u;
  return pi_cplugins_manifest_json;
}

static int32_t pi_cplugins_generated_call(pi_plugin_api* api,
                                          const uint8_t* input,
                                          size_t input_size,
                                          uint8_t* output,
                                          size_t output_capacity,
                                          size_t* output_size) {
  if (api == NULL || output_size == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  if (input_size > api->max_payload_bytes) return PI_PLUGIN_ERR_PAYLOAD_TOO_LARGE;
  if (input == NULL || input_size < 4u) return PI_PLUGIN_ERR_INVALID_ARGUMENT;

  const uint8_t* pi_cursor = input;
  const uint8_t* pi_end = input + input_size;
  uint32_t pi_function_id;
  if (!pi_cplugins_read_u32(&pi_cursor, pi_end, &pi_function_id)) {
    return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }

  switch (pi_function_id) {
${cases}
    default:
      return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  }
}

static int32_t pi_cplugins_generated_close(pi_plugin_api* api) {
  if (api == NULL) return PI_PLUGIN_ERR_INVALID_ARGUMENT;
  pi_cplugins_generated_api* plugin = (pi_cplugins_generated_api*)api;
  pi_plugin_host_api host = plugin->host;
  host.free(host.host_data, plugin);
  return PI_PLUGIN_OK;
}

pi_plugin_api* pi_plugin_init(const pi_plugin_host_api* host) {
  if (host == NULL || host->malloc == NULL || host->free == NULL) return NULL;
  pi_cplugins_generated_api* plugin =
      (pi_cplugins_generated_api*)host->malloc(host->host_data, sizeof(*plugin));
  if (plugin == NULL) return NULL;
  memset(plugin, 0, sizeof(*plugin));
  pi_plugin_copy_host_api(&plugin->host, host);
  plugin->base.max_payload_bytes = PI_CPLUGINS_GENERATED_MAX_PAYLOAD;
  plugin->base.manifest = pi_cplugins_generated_manifest;
  plugin->base.call = pi_cplugins_generated_call;
  plugin->base.close = pi_cplugins_generated_close;
  return &plugin->base;
}
`;

  return {
    source: `#line 1 "${escapedSourceName}"\n${source}\n#line 1 "pi-generated-bindings.c"\n${adapter}`,
    manifest,
  };
}
