#include <stdbool.h>
#include <stdint.h>

static int32_t label_calls = 0;

static int32_t clamp(int32_t value, int32_t low, int32_t high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

int32_t add(int32_t left, int32_t right) {
  return left + right;
}

double affine(double value, double scale, double offset) {
  return value * scale + offset;
}

bool in_range(int32_t value, int32_t low, int32_t high) {
  return clamp(value, low, high) == value;
}

const char* echo(const char* value) {
  return value;
}

const char* counted_label(void) {
  label_calls++;
  return "TinyCC module";
}

int32_t counted_label_calls(void) {
  return label_calls;
}

uint64_t next_u64(uint64_t value) {
  return value + 1u;
}
