import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import test from "node:test";
import { crc32 } from "node:zlib";

import { parseBindings } from "../src/c-parser.js";
import { cc } from "../src/cc.js";
import {
  compileNativePlugin,
  pointer,
  publicIncludePath,
  readPointer,
} from "../src/native.js";
import { CString, toArrayBuffer } from "../src/ffi.js";
import { compileCFile } from "../src/runtime.js";

const root = resolve(import.meta.dirname, "..");
const ergonomicPath = resolve(root, "examples/ergonomic.c");

test("Tree-sitter discovers supported public C functions", async () => {
  const source = await readFile(ergonomicPath, "utf8");
  const bindings = await parseBindings(source);
  assert.deepEqual(
    bindings.map((binding) => binding.name),
    [
      "add",
      "affine",
      "in_range",
      "echo",
      "counted_label",
      "counted_label_calls",
      "next_u64",
    ],
  );
  assert.deepEqual(bindings[0].parameters.map((parameter) => parameter.kind), ["i32", "i32"]);
  assert.equal(bindings[3].returnKind, "cstring");
  assert.equal(bindings[6].returnKind, "u64");
});

test("Tree-sitter infers pointers, ABI-value composites, enums, and callbacks", async () => {
  const bindings = await parseBindings(`
    struct pair { int x; int y; };
    enum side { LEFT = -1, RIGHT = 1 };
    int32_t read_pointer(const int32_t *value) { return *value; }
    struct pair read_pair(void) { return (struct pair){1, 2}; }
    enum side flip(enum side value) { return value == LEFT ? RIGHT : LEFT; }
    int32_t apply(int32_t value, int32_t (*callback)(int32_t)) { return callback(value); }
  `);
  assert.equal(bindings[0].parameters[0].kind, "ptr");
  assert.equal(bindings[1].returnKind, "struct:pair");
  assert.equal(bindings[2].parameters[0].kind, "enum:side");
  assert.deepEqual(bindings[3].parameters[1].callback, {
    returnCType: "int32_t",
    returnKind: "i32",
    parameters: [{ cType: "int32_t", kind: "i32" }],
  });
});

test("Node binding compiles and runs the handwritten ABI plugin with TinyCC", async () => {
  const source = await readFile(resolve(root, "fixture/conformance_plugin.c"), "utf8");
  const plugin = compileNativePlugin(source, [publicIncludePath()]);
  try {
    assert.match(plugin.manifest(), /pi-cplugins-conformance-fixture/);
    assert.equal(plugin.call(Buffer.from("cplugins")).toString(), "CPLUGINS");
    assert.throws(() => plugin.call(Buffer.alloc(129)), /payload bound/);
  } finally {
    plugin.close();
  }
  assert.throws(() => plugin.manifest(), /closed/);
});

test("generated bindings call plain C with ergonomic JavaScript values", async () => {
  const module = await compileCFile(ergonomicPath);
  try {
    assert.equal(module.call("add", [20, 22]), 42);
    assert.equal(module.call("affine", [3, 2.5, 1]), 8.5);
    assert.equal(module.call("in_range", [5, 1, 9]), true);
    assert.equal(module.call("in_range", [12, 1, 9]), false);
    assert.equal(module.call("echo", ["hello from TinyCC"]), "hello from TinyCC");
    assert.equal(module.call("counted_label", []), "TinyCC module");
    assert.equal(module.call("counted_label_calls", []), 1);
    assert.equal(module.call("next_u64", ["18446744073709551614"]), 18446744073709551615n);
    assert.throws(() => module.call("add", [1]), /expects 2 arguments/);
  } finally {
    module.close();
  }
  module.close();
});

test("TinyCC thunks handle pointers, arrays, ABI-value composites, enums, and callbacks", async () => {
  const module = await compileCFile(resolve(root, "examples/ffi.c"));
  try {
    const values = new Float64Array([1.5, 2.5, 4]);
    assert.equal(module.call("scale_f64", [values, values.length, 2]), undefined);
    assert.deepEqual([...values], [3, 5, 8]);

    const reversed = module.callDetailed("reverse4", [[1, 2, 3, 4]]);
    assert.deepEqual(reversed.mutableArguments.values, [4, 3, 2, 1]);

    const text = Buffer.from("Tiny c ffi");
    module.call("uppercase_ascii", [text, text.length]);
    assert.equal(text.toString(), "TINY C FFI");
    assert.equal(module.call("sum_bytes", [text, text.length]), 668);

    const point = new Float64Array([3, 4]);
    assert.equal(module.call("point_norm_squared", [point]), 25);

    assert.deepEqual(module.layout("struct:point"), { size: 16, alignment: 8 });
    assert.equal(module.allocate("struct:point").length, 16);
    const pointValue = module.call("make_point", [3, 4]);
    assert.ok(Buffer.isBuffer(pointValue));
    assert.equal(pointValue.length, 16);
    assert.equal(module.call("point_value_norm_squared", [pointValue]), 25);

    const numberBits = module.call("make_number_bits", [0x3f800000]);
    assert.ok(Buffer.isBuffer(numberBits));
    assert.equal(numberBits.length, 4);
    assert.equal(module.call("number_bits_real", [numberBits]), 1);
    assert.equal(module.call("reverse_direction", [-1]), 1);
    assert.equal(module.call("apply_i32", [14, (value: unknown) => Number(value) * 3]), 42);
    assert.throws(
      () => module.call("apply_i32", [14, () => { throw new Error("callback failed"); }]),
      /callback failed/,
    );

    const bytes = Uint8Array.from([10, 20, 30, 40]);
    assert.deepEqual([...readPointer(pointer(bytes), bytes.byteLength)], [...bytes]);
  } finally {
    module.close();
  }
});

test("CString and toArrayBuffer expose TinyCC pointer memory", () => {
  const text = Buffer.concat([Buffer.from("pointer string"), Buffer.from([0])]);
  assert.equal(CString(pointer(text)), "pointer string");
  const shared = new Uint8Array(toArrayBuffer(pointer(text), 0, text.length));
  shared[0] = "P".charCodeAt(0);
  assert.equal(text.toString("utf8", 0, 7), "Pointer");
});

test("cc supports explicit symbols, defines, flags, and variadic calls", async () => {
  const compiled = await cc({
    source: resolve(root, "examples/cc.c"),
    define: { PI_CC_FACTOR: "7" },
    flags: "-Wall",
    symbols: {
      configured_answer: { args: ["int"], returns: "int32_t" },
      call_transform: {
        args: ["i32", { callback: { args: ["i32"], returns: "i32" } }],
        returns: "i32",
      },
      has_napi_env: { args: ["napi_env"], returns: "i32" },
      identity_napi_value: { args: ["napi_value"], returns: "napi_value" },
      make_napi_answer: { args: ["napi_env"], returns: "napi_value" },
      sum_promoted_f32: {
        args: ["i32", "f32", "f32"],
        returns: "f64",
      },
      sum_promoted_small: {
        args: ["i32", "i8", "u16", "bool"],
        returns: "i32",
      },
      sum_variadic: {
        args: ["i32", "i32", "i32", "i32"],
        returns: "i32",
      },
    },
  });
  try {
    assert.equal(compiled.symbols.configured_answer(6), 42);
    assert.equal(compiled.symbols.call_transform(21, (value: unknown) => Number(value) * 2), 42);
    assert.equal(compiled.symbols.has_napi_env(), 1);
    const identity = { answer: 42 };
    assert.equal(compiled.symbols.identity_napi_value(identity), identity);
    assert.equal(compiled.symbols.make_napi_answer(), 42);
    assert.equal(compiled.symbols.sum_promoted_f32(2, 1.5, 2.25), 3.75);
    assert.equal(compiled.symbols.sum_promoted_small(3, -2, 40, true), 39);
    assert.equal(compiled.symbols.sum_variadic(3, 10, 20, 12), 42);
  } finally {
    compiled.close();
  }
});

test("cc links requested libraries through TinyCC", async () => {
  const libraryDirectory = resolve(root, ".build/ffi-library");
  const objectPath = resolve(libraryDirectory, "answer.o");
  const libraryPath = resolve(libraryDirectory, "libanswer.a");
  const tcc = resolve(root, ".deps/tinycc/prefix/bin/tcc");
  mkdirSync(libraryDirectory, { recursive: true });
  execFileSync(tcc, [
    "-c",
    resolve(root, "examples/library_provider.c"),
    "-o",
    objectPath,
  ]);
  execFileSync(tcc, ["-ar", "rcs", libraryPath, objectPath]);

  const compiled = await cc({
    source: resolve(root, "examples/library_user.c"),
    flags: `-L${libraryDirectory}`,
    library: "answer",
    symbols: {
      call_linked_library: { args: [], returns: "i32" },
    },
  });
  try {
    assert.equal(compiled.symbols.call_linked_library(), 42);
  } finally {
    compiled.close();
  }
});

test("cc links the host zlib and passes native buffer memory", async () => {
  const compiled = await cc({
    source: resolve(root, "examples/zlib.c"),
    library: "z",
  });
  try {
    const bytes = Buffer.from("Pi agents execute real C libraries");
    assert.match(compiled.symbols.linked_zlib_version() as string, /^\d+\.\d+/);
    assert.equal(
      compiled.symbols.linked_zlib_crc32(bytes, bytes.length),
      crc32(bytes),
    );
  } finally {
    compiled.close();
  }
});

test("TinyCC diagnostics reach JavaScript", () => {
  assert.throws(
    () => compileNativePlugin("int broken( {", [publicIncludePath()]),
    /error|expected|declaration/i,
  );
});
