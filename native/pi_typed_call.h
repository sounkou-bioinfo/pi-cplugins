#ifndef PI_TYPED_CALL_H
#define PI_TYPED_CALL_H

#include <stddef.h>
#include <stdint.h>

#define PI_TYPED_JS_CALLBACK ((size_t)-1)

typedef struct pi_typed_value {
  union {
    void* pointer;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
  } value;
  size_t byte_length;
} pi_typed_value;

typedef int32_t (*pi_typed_call_fn)(void* host_context,
                                    const pi_typed_value* arguments,
                                    size_t argument_count,
                                    pi_typed_value* result);

#endif
