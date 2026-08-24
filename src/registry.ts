import {
  compileCFile,
  type BindingArgument,
  type BoundCallResult,
  type BoundCModule,
} from "./runtime.js";
import type { NativeCompileOptions } from "./native.js";

export interface ModuleSummary {
  id: string;
  sourcePath: string;
  functions: string[];
}

interface RegistryEntry {
  sourcePath: string;
  module: BoundCModule;
}

export class CModuleRegistry {
  #modules = new Map<string, RegistryEntry>();

  async load(
    id: string,
    sourcePath: string,
    options: NativeCompileOptions = {},
  ): Promise<ModuleSummary> {
    if (!/^[A-Za-z_][A-Za-z0-9_-]*$/.test(id)) {
      throw new Error("module id must start with a letter or underscore and contain only letters, digits, _ or -");
    }
    if (this.#modules.has(id)) throw new Error(`C module '${id}' is already loaded`);

    const module = await compileCFile(sourcePath, options);
    this.#modules.set(id, { sourcePath, module });
    return this.summary(id, sourcePath, module);
  }

  call(id: string, functionName: string, args: BindingArgument[]): BoundCallResult {
    const entry = this.#modules.get(id);
    if (!entry) throw new Error(`C module '${id}' is not loaded`);
    return entry.module.callDetailed(functionName, args);
  }

  unload(id: string): void {
    const entry = this.#modules.get(id);
    if (!entry) throw new Error(`C module '${id}' is not loaded`);
    entry.module.close();
    this.#modules.delete(id);
  }

  list(): ModuleSummary[] {
    return [...this.#modules].map(([id, entry]) => this.summary(id, entry.sourcePath, entry.module));
  }

  closeAll(): void {
    for (const entry of this.#modules.values()) entry.module.close();
    this.#modules.clear();
  }

  private summary(id: string, sourcePath: string, module: BoundCModule): ModuleSummary {
    return {
      id,
      sourcePath,
      functions: module.functions.map((fn) => fn.name),
    };
  }
}
