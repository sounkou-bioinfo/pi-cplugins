import { readFile } from "node:fs/promises";
import { basename, dirname, resolve } from "node:path";

import type { BindingFunction, BindingParameter } from "./c-parser.js";
import { parseBindings } from "./c-parser.js";
import { generatePluginSource } from "./c-generator.js";
import {
  callTyped,
  compileNativePlugin,
  nativeIncludePath,
  nodeApiIncludePath,
  publicIncludePath,
  type CompositeFFIType,
  type FFIType,
  type NativeCompileOptions,
  type NativePlugin,
} from "./native.js";

export type BindingArgument = unknown;
export type BindingResult = unknown;

export interface CompositeLayout {
  readonly size: number;
  readonly alignment: number;
}

export interface BoundCallResult {
  value: BindingResult;
  mutableArguments: Record<string, Array<number | string>>;
}

function pointerArray(parameter: BindingParameter, value: unknown): ArrayBufferView | undefined {
  if (!Array.isArray(value)) return undefined;
  const numbers = value.map((item) => {
    if (typeof item !== "number" || !Number.isFinite(item)) {
      throw new Error(`${parameter.name} pointer array must contain finite numbers`);
    }
    return item;
  });
  switch (parameter.pointee) {
    case "int8_t":
    case "signed char":
      return Int8Array.from(numbers);
    case "char":
    case "uint8_t":
    case "unsigned char":
      return Uint8Array.from(numbers);
    case "int16_t":
    case "short":
    case "short int":
      return Int16Array.from(numbers);
    case "uint16_t":
    case "unsigned short":
    case "unsigned short int":
      return Uint16Array.from(numbers);
    case "int32_t":
    case "int":
      return Int32Array.from(numbers);
    case "uint32_t":
    case "unsigned int":
      return Uint32Array.from(numbers);
    case "int64_t":
      return BigInt64Array.from(numbers.map(BigInt));
    case "uint64_t":
    case "size_t":
      return BigUint64Array.from(numbers.map(BigInt));
    case "float":
      return Float32Array.from(numbers);
    case "double":
      return Float64Array.from(numbers);
    default:
      return Uint8Array.from(numbers);
  }
}

function mutableSnapshot(view: ArrayBufferView): Array<number | string> {
  if (view instanceof DataView) {
    return [...new Uint8Array(view.buffer, view.byteOffset, view.byteLength)];
  }
  const values = view as ArrayBufferView & {
    readonly length: number;
    readonly [index: number]: number | bigint;
  };
  return Array.from({ length: values.length }, (_, index) => {
    const value = values[index];
    return typeof value === "bigint" ? value.toString() : value;
  });
}

export class BoundCModule {
  readonly name: string;
  readonly functions: readonly BindingFunction[];
  readonly manifest: string;
  #native: NativePlugin | undefined;
  #byName: Map<string, BindingFunction>;
  #symbols = new Map<string, number>();
  #layouts = new Map<CompositeFFIType, CompositeLayout>();

  constructor(name: string, functions: BindingFunction[], native: NativePlugin) {
    this.name = name;
    this.functions = functions;
    this.#native = native;
    this.#byName = new Map(functions.map((fn) => [fn.name, fn]));
    this.manifest = native.manifest();
  }

  symbolPointer(functionName: string): number {
    if (!this.#native) throw new Error(`C module '${this.name}' is closed`);
    const fn = this.#byName.get(functionName);
    if (!fn) {
      throw new Error(`C module '${this.name}' has no function '${functionName}'`);
    }
    let symbol = this.#symbols.get(functionName);
    if (symbol === undefined) {
      symbol = this.#native.symbol(`pi_cplugins_typed_${fn.id}`);
      this.#symbols.set(functionName, symbol);
    }
    return symbol;
  }

  layout(type: CompositeFFIType): CompositeLayout {
    if (!this.#native) throw new Error(`C module '${this.name}' is closed`);
    if (!/^(?:struct|union):[A-Za-z_][A-Za-z0-9_]*$/.test(type)) {
      throw new Error(`invalid composite type '${type}'`);
    }
    const cached = this.#layouts.get(type);
    if (cached) return cached;
    const suffix = type.replace(":", "_");
    const query = (operation: "sizeof" | "alignof") => callTyped(
      this.#native!.symbol(`pi_cplugins_${operation}_${suffix}`),
      [],
      "u64_fast",
      [],
    ) as number;
    const layout = Object.freeze({ size: query("sizeof"), alignment: query("alignof") });
    this.#layouts.set(type, layout);
    return layout;
  }

  allocate(type: CompositeFFIType): Buffer {
    return Buffer.alloc(this.layout(type).size);
  }

  call(functionName: string, args: BindingArgument[]): BindingResult {
    return this.callDetailed(functionName, args).value;
  }

  callDetailed(functionName: string, args: BindingArgument[]): BoundCallResult {
    if (!this.#native) throw new Error(`C module '${this.name}' is closed`);
    const fn = this.#byName.get(functionName);
    if (!fn) throw new Error(`C module '${this.name}' has no function '${functionName}'`);
    const visibleParameters = fn.parameters.filter((parameter) => parameter.kind !== "napi_env");
    if (args.length !== visibleParameters.length) {
      throw new Error(`${functionName} expects ${visibleParameters.length} arguments, received ${args.length}`);
    }

    const nativeArguments: unknown[] = [];
    const mutableViews = new Map<string, ArrayBufferView>();
    let visibleIndex = 0;
    for (const parameter of fn.parameters) {
      if (parameter.kind === "napi_env") continue;
      const value = args[visibleIndex++];
      let converted =
        parameter.kind === "ptr" || parameter.kind === "buffer"
          ? pointerArray(parameter, value) ?? value
          : value;
      if (
        (parameter.kind === "i64" ||
          parameter.kind === "i64_fast" ||
          parameter.kind === "u64" ||
          parameter.kind === "u64_fast") &&
        typeof converted === "string" &&
        /^-?\d+$/.test(converted)
      ) {
        converted = BigInt(converted);
      }
      nativeArguments.push(converted);
      if (parameter.mutable && ArrayBuffer.isView(converted)) {
        mutableViews.set(parameter.name, converted);
      }
    }

    const value = callTyped(
      this.symbolPointer(functionName),
      fn.parameters.map((parameter) => parameter.kind as FFIType),
      fn.returnKind as FFIType,
      nativeArguments,
    );
    return {
      value,
      mutableArguments: Object.fromEntries(
        [...mutableViews].map(([name, view]) => [name, mutableSnapshot(view)]),
      ),
    };
  }

  close(): void {
    if (!this.#native) return;
    this.#native.close();
    this.#native = undefined;
    this.#symbols.clear();
    this.#layouts.clear();
  }
}

export function compileCSource(
  source: string,
  sourceName: string,
  functions: BindingFunction[],
  includePaths: string[],
  options: NativeCompileOptions = {},
): BoundCModule {
  const generated = generatePluginSource(source, functions, sourceName);
  const native = compileNativePlugin(
    generated.source,
    [publicIncludePath(), nativeIncludePath(), nodeApiIncludePath(), ...includePaths],
    options,
  );
  const nativeManifest = JSON.parse(native.manifest()) as { functions?: BindingFunction[] };
  if (JSON.stringify(nativeManifest.functions) !== JSON.stringify(functions)) {
    native.close();
    throw new Error("generated plugin manifest drifted from Tree-sitter binding metadata");
  }
  return new BoundCModule(sourceName, functions, native);
}

export async function compileCFile(
  path: string,
  options: NativeCompileOptions = {},
): Promise<BoundCModule> {
  const absolutePath = resolve(path);
  const source = await readFile(absolutePath, "utf8");
  const functions = await parseBindings(source);
  return compileCSource(
    source,
    basename(absolutePath),
    functions,
    [dirname(absolutePath)],
    options,
  );
}
