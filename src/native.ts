import { createRequire } from "node:module";
import { basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export type CompositeFFIType =
  | `struct:${string}`
  | `union:${string}`
  | `enum:${string}`;

export type FFIType =
  | "void"
  | "buffer"
  | "buffer_length"
  | "cstring"
  | "function"
  | "ptr"
  | "i8"
  | "i16"
  | "i32"
  | "i64"
  | "i64_fast"
  | "u8"
  | "u16"
  | "u32"
  | "u64"
  | "u64_fast"
  | "f32"
  | "f64"
  | "bool"
  | "char"
  | "napi_env"
  | "napi_value"
  | CompositeFFIType;

export type FFITypeInput =
  | FFIType
  | "fn"
  | "callback"
  | "pointer"
  | "void*"
  | "char*"
  | "int8_t"
  | "int16_t"
  | "int32_t"
  | "int"
  | "int64_t"
  | "isize"
  | "uint8_t"
  | "uint16_t"
  | "uint32_t"
  | "uint64_t"
  | "usize"
  | "float"
  | "double";

export interface NativeExtensionRegistration {
  readonly id: number;
  readonly kind: "tool" | "command" | "event";
  readonly name: string;
  readonly label?: string;
  readonly description?: string;
  readonly parametersJson?: string;
}

export interface NativeExtensionBridge {
  isCancelled(): boolean;
  update(json: string): void;
  notify(message: string, level: number): void;
  setStatus(key: string, text: string | null): void;
  setWidget(key: string, linesJson: string | null, placement: number): void;
}

export interface NativePlugin {
  manifest(): string;
  call(input: Buffer): Buffer;
  symbol(name: string): number;
  registrations(): NativeExtensionRegistration[];
  invoke(id: number, inputJson: string, cwd: string, bridge: NativeExtensionBridge): string | undefined;
  close(): void;
}

export interface NativeCompileOptions {
  flags?: string | string[];
  library?: string | string[];
  define?: Record<string, string>;
}

interface NativeAddon {
  compile(
    source: string,
    includePaths: string[],
    flags: string[],
    libraries: string[],
    defines: Array<[string, string]>,
  ): NativePlugin;
  typedCall(
    pointer: number,
    args: FFIType[],
    returns: FFIType,
    values: unknown[],
  ): unknown;
  pointer(value: ArrayBufferView): number;
  readPointer(pointer: number | bigint, byteLength: number, byteOffset?: number): Buffer;
  cstring(pointer: number | bigint, byteOffset?: number, byteLength?: number): string;
  toArrayBuffer(
    pointer: number | bigint,
    byteOffset: number,
    byteLength: number | null,
    deallocatorContext: number | bigint | null,
    deallocator: number | bigint | null,
  ): ArrayBuffer;
}

const require = createRequire(import.meta.url);
const moduleParent = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = basename(moduleParent) === "dist"
  ? resolve(moduleParent, "..")
  : moduleParent;
const nodeApiInclude = resolve(
  dirname(require.resolve("node-api-headers/package.json")),
  "include",
);

let loadedAddon: NativeAddon | undefined;

function addon(): NativeAddon {
  if (!loadedAddon) {
    const addonPath = resolve(projectRoot, "build/Release/pi_tinycc.node");
    try {
      loadedAddon = require(addonPath) as NativeAddon;
    } catch (error) {
      throw new Error(
        `TinyCC Node binding is unavailable at ${addonPath}; run npm run build:native`,
        { cause: error },
      );
    }
  }
  return loadedAddon;
}

function stringArray(value: string | string[] | undefined): string[] {
  if (value === undefined) return [];
  return Array.isArray(value) ? value : [value];
}

export function compileNativePlugin(
  source: string,
  includePaths: string[],
  options: NativeCompileOptions = {},
): NativePlugin {
  return addon().compile(
    source,
    includePaths,
    stringArray(options.flags),
    stringArray(options.library),
    Object.entries(options.define ?? {}),
  );
}

export function normalizeFFIType(kind: FFITypeInput): FFIType {
  const aliases: Partial<Record<FFITypeInput, FFIType>> = {
    fn: "function",
    callback: "function",
    pointer: "ptr",
    "void*": "ptr",
    "char*": "ptr",
    int8_t: "i8",
    int16_t: "i16",
    int32_t: "i32",
    int: "i32",
    int64_t: "i64",
    isize: "i64",
    uint8_t: "u8",
    uint16_t: "u16",
    uint32_t: "u32",
    uint64_t: "u64",
    usize: "u64",
    float: "f32",
    double: "f64",
  };
  const normalized = aliases[kind] ?? (kind as FFIType);
  if (/^(?:struct|union|enum):[A-Za-z_][A-Za-z0-9_]*$/.test(normalized)) {
    return normalized;
  }
  const known = new Set<FFIType>([
    "void", "buffer", "buffer_length", "cstring", "function", "ptr",
    "i8", "i16", "i32", "i64", "i64_fast", "u8", "u16", "u32",
    "u64", "u64_fast", "f32", "f64", "bool", "char", "napi_env",
    "napi_value",
  ]);
  if (!known.has(normalized)) throw new Error(`unsupported C binding type '${kind}'`);
  return normalized;
}

export function callTyped(
  pointer: number,
  args: FFITypeInput[],
  returns: FFITypeInput,
  values: unknown[],
): unknown {
  return addon().typedCall(
    pointer,
    args.map(normalizeFFIType),
    normalizeFFIType(returns),
    values,
  );
}

export function pointer(value: ArrayBufferView): number {
  return addon().pointer(value);
}

export function readPointer(
  address: number | bigint,
  byteLength: number,
  byteOffset = 0,
): Buffer {
  return addon().readPointer(address, byteLength, byteOffset);
}

export function cstring(
  address: number | bigint,
  byteOffset = 0,
  byteLength?: number,
): string {
  return byteLength === undefined
    ? addon().cstring(address, byteOffset)
    : addon().cstring(address, byteOffset, byteLength);
}

export function externalArrayBuffer(
  address: number | bigint,
  byteOffset: number,
  byteLength: number | null,
  deallocatorContext: number | bigint | null,
  deallocator: number | bigint | null,
): ArrayBuffer {
  return addon().toArrayBuffer(
    address,
    byteOffset,
    byteLength,
    deallocatorContext,
    deallocator,
  );
}

export function publicIncludePath(): string {
  return resolve(projectRoot, "include");
}

export function nativeIncludePath(): string {
  return resolve(projectRoot, "native");
}

export function nodeApiIncludePath(): string {
  return nodeApiInclude;
}
