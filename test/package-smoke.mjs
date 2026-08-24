import assert from "node:assert/strict";
import { resolve } from "node:path";
import { crc32 } from "node:zlib";

import { cc } from "pi-cplugins";

const compiled = await cc({ source: resolve("examples/ergonomic.c") });
try {
  assert.equal(compiled.symbols.add(20, 22), 42);
} finally {
  compiled.close();
}

const zlib = await cc({ source: resolve("examples/zlib.c"), library: "z" });
try {
  const bytes = Buffer.from("packed pi-cplugins links zlib");
  assert.equal(zlib.symbols.linked_zlib_crc32(bytes, bytes.length), crc32(bytes));
} finally {
  zlib.close();
}
