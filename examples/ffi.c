#include <stddef.h>
#include <stdint.h>

struct point {
  double x;
  double y;
};

union number_bits {
  int32_t integer;
  float real;
};

enum direction {
  DIRECTION_LEFT = -1,
  DIRECTION_RIGHT = 1
};

void scale_f64(double* values, size_t length, double factor) {
  for (size_t i = 0u; i < length; ++i) values[i] *= factor;
}

void reverse4(int32_t values[4]) {
  const int32_t first = values[0];
  const int32_t second = values[1];
  values[0] = values[3];
  values[1] = values[2];
  values[2] = second;
  values[3] = first;
}

void uppercase_ascii(char* text, size_t length) {
  for (size_t i = 0u; i < length; ++i) {
    if (text[i] >= 'a' && text[i] <= 'z') text[i] = (char)(text[i] - ('a' - 'A'));
  }
}

uint32_t sum_bytes(const uint8_t* bytes, size_t length) {
  uint32_t sum = 0u;
  for (size_t i = 0u; i < length; ++i) sum += bytes[i];
  return sum;
}

double point_norm_squared(const struct point* value) {
  return value->x * value->x + value->y * value->y;
}

struct point make_point(double x, double y) {
  const struct point value = {x, y};
  return value;
}

double point_value_norm_squared(struct point value) {
  return value.x * value.x + value.y * value.y;
}

union number_bits make_number_bits(int32_t value) {
  union number_bits result;
  result.integer = value;
  return result;
}

float number_bits_real(union number_bits value) {
  return value.real;
}

enum direction reverse_direction(enum direction value) {
  return value == DIRECTION_LEFT ? DIRECTION_RIGHT : DIRECTION_LEFT;
}

int32_t apply_i32(int32_t value, int32_t (*callback)(int32_t)) {
  return callback(value);
}
