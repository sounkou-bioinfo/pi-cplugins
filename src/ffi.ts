import {
  cstring,
  externalArrayBuffer,
  pointer,
  type CompositeFFIType,
  type FFIType,
  type FFITypeInput,
} from "./native.js";

export function ptr(value: ArrayBufferView): number {
  return pointer(value);
}

export function CString(
  address: number | bigint,
  byteOffset = 0,
  byteLength?: number,
): string {
  return cstring(address, byteOffset, byteLength);
}

export function toArrayBuffer(
  address: number | bigint,
  byteOffset = 0,
  byteLength?: number,
  deallocatorContext: number | bigint | null = null,
  deallocator: number | bigint | null = null,
): ArrayBuffer {
  if (deallocator === null && deallocatorContext !== null) {
    deallocator = deallocatorContext;
    deallocatorContext = null;
  }
  return externalArrayBuffer(
    address,
    byteOffset,
    byteLength ?? null,
    deallocatorContext,
    deallocator,
  );
}

export function toBuffer(
  address: number | bigint,
  byteOffset = 0,
  byteLength?: number,
): Buffer {
  return Buffer.from(toArrayBuffer(address, byteOffset, byteLength));
}

function dataView(address: number | bigint, byteOffset: number, size: number): DataView {
  return new DataView(toArrayBuffer(address, byteOffset, size));
}

export const read = {
  ptr: (address: number | bigint, byteOffset = 0) =>
    Number(dataView(address, byteOffset, 8).getBigUint64(0, true)),
  i8: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 1).getInt8(0),
  i16: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 2).getInt16(0, true),
  i32: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 4).getInt32(0, true),
  i64: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 8).getBigInt64(0, true),
  u8: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 1).getUint8(0),
  u16: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 2).getUint16(0, true),
  u32: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 4).getUint32(0, true),
  u64: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 8).getBigUint64(0, true),
  f32: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 4).getFloat32(0, true),
  f64: (address: number | bigint, byteOffset = 0) =>
    dataView(address, byteOffset, 8).getFloat64(0, true),
};

export type { CompositeFFIType, FFIType, FFITypeInput };
