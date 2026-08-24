import { readFile } from "node:fs/promises";
import { basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import type {
  BindingFunction,
  BindingKind,
  BindingParameter,
  CallbackSignature,
} from "./c-parser.js";
import { parseBindings } from "./c-parser.js";
import {
  normalizeFFIType,
  type FFIType,
  type FFITypeInput,
  type NativeCompileOptions,
} from "./native.js";
import {
  compileCSource,
  type BoundCModule,
  type CompositeLayout,
} from "./runtime.js";

export interface CallbackDescriptor {
  callback: {
    args: FFITypeInput[];
    returns: FFITypeInput;
  };
}

export interface SymbolDescriptor {
  args: Array<FFITypeInput | CallbackDescriptor>;
  returns: FFITypeInput;
}

export interface CCOptions extends NativeCompileOptions {
  source: string | URL;
  symbols?: Record<string, SymbolDescriptor>;
}

export interface CCResult {
  readonly symbols: Record<string, (...args: unknown[]) => unknown>;
  readonly module: BoundCModule;
  layout(type: `struct:${string}` | `union:${string}`): CompositeLayout;
  allocate(type: `struct:${string}` | `union:${string}`): Buffer;
  close(): void;
}

function sourcePath(source: string | URL): string {
  if (source instanceof URL) {
    if (source.protocol !== "file:") throw new Error("cc source URL must use file:");
    return fileURLToPath(source);
  }
  return resolve(source);
}

function descriptorCType(kind: BindingKind): string {
  if (kind.startsWith("struct:") || kind.startsWith("union:") || kind.startsWith("enum:")) {
    return kind.replace(":", " ");
  }
  const types: Record<string, string> = {
    void: "void", bool: "_Bool", char: "char", i8: "int8_t", i16: "int16_t",
    i32: "int32_t", i64: "int64_t", i64_fast: "int64_t", u8: "uint8_t",
    u16: "uint16_t", u32: "uint32_t", u64: "uint64_t", u64_fast: "uint64_t",
    f32: "float", f64: "double", cstring: "const char*", buffer: "char*",
    ptr: "void*", napi_env: "napi_env", napi_value: "napi_value",
    function: "void*", buffer_length: "size_t",
  };
  return types[kind] ?? kind;
}

function descriptorCallback(value: CallbackDescriptor, context: string): CallbackSignature {
  const parameters = value.callback.args.map((input) => {
    const kind = normalizeFFIType(input);
    if (kind === "void" || kind === "function" || kind === "napi_env" ||
        kind === "napi_value") {
      throw new Error(`${context} callback uses unsupported argument type '${kind}'`);
    }
    return { kind, cType: descriptorCType(kind) };
  });
  const returnKind = normalizeFFIType(value.callback.returns);
  if (returnKind === "function" || returnKind === "napi_env" ||
      returnKind === "napi_value" || returnKind === "cstring") {
    throw new Error(`${context} callback uses unsupported result type '${returnKind}'`);
  }
  return {
    returnKind,
    returnCType: descriptorCType(returnKind),
    parameters,
  } as CallbackSignature;
}

function descriptorBindings(symbols: Record<string, SymbolDescriptor>): BindingFunction[] {
  return Object.entries(symbols).map(([name, descriptor], id) => {
    const parameters: BindingParameter[] = descriptor.args.map((input, index) => {
      if (typeof input === "object") {
        return {
          name: `arg${index}`,
          cType: "function",
          kind: "function",
          callback: descriptorCallback(input, `${name}.args[${index}]`),
          mutable: false,
        };
      }
      const kind = normalizeFFIType(input);
      if (kind === "void") throw new Error(`${name} uses void as an argument type`);
      return {
        name: `arg${index}`,
        cType: descriptorCType(kind),
        kind: kind as Exclude<FFIType, "void">,
        mutable: kind === "buffer" || kind === "ptr",
      };
    });
    const args = parameters.map((parameter) => parameter.kind);
    const returns = normalizeFFIType(descriptor.returns);
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
      throw new Error(`invalid C symbol name '${name}'`);
    }
    if (args.some((kind) => kind === "buffer_length")) {
      throw new Error(`${name} uses buffer_length, which is unavailable to cc`);
    }
    return {
      id,
      name,
      returnCType: descriptorCType(returns),
      returnKind: returns,
      parameters,
    };
  });
}

export async function cc(options: CCOptions): Promise<CCResult> {
  const path = sourcePath(options.source);
  const source = await readFile(path, "utf8");
  const functions = options.symbols
    ? descriptorBindings(options.symbols)
    : await parseBindings(source);
  const module = compileCSource(
    source,
    basename(path),
    functions,
    [dirname(path)],
    {
      flags: options.flags,
      library: options.library,
      define: options.define,
    },
  );
  const symbols = Object.fromEntries(
    functions.map((fn) => [fn.name, (...args: unknown[]) => module.call(fn.name, args)]),
  );
  return {
    symbols,
    module,
    layout: (type) => module.layout(type),
    allocate: (type) => module.allocate(type),
    close: () => module.close(),
  };
}
