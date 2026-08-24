import { readFile, realpath } from "node:fs/promises";
import { basename, extname, isAbsolute, relative, resolve, sep } from "node:path";

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type, type Static } from "typebox";

import { compilePiExtension, type BoundPiExtension } from "../src/pi-extension.js";
import { CModuleRegistry } from "../src/registry.js";

const argumentSchema = Type.Union([
  Type.Boolean(),
  Type.Number(),
  Type.String(),
  Type.Array(Type.Number()),
]);
const stringList = Type.Union([Type.String(), Type.Array(Type.String())]);
const loadSchema = Type.Object({
  path: Type.String({ description: "C source file inside the Pi working directory" }),
  module: Type.Optional(Type.String({ description: "Module id; defaults to the source filename" })),
  library: Type.Optional(stringList),
  flags: Type.Optional(stringList),
  define: Type.Optional(Type.Record(Type.String(), Type.String())),
});
const callSchema = Type.Object({
  module: Type.String({ description: "Loaded C module id" }),
  function: Type.String({ description: "Tree-sitter-discovered C function name" }),
  arguments: Type.Array(argumentSchema, { description: "Typed arguments in declaration order" }),
});
const extensionSchema = Type.Object({
  path: Type.String({ description: "C Pi extension source inside the working directory" }),
  library: Type.Optional(stringList),
  flags: Type.Optional(stringList),
  define: Type.Optional(Type.Record(Type.String(), Type.String())),
});
const moduleSchema = Type.Object({
  module: Type.String({ description: "Loaded C module id" }),
});

type LoadInput = Static<typeof loadSchema>;
type CallInput = Static<typeof callSchema>;
type ExtensionInput = Static<typeof extensionSchema>;
type ModuleInput = Static<typeof moduleSchema>;

function defaultModuleId(path: string): string {
  const filename = basename(path, extname(path));
  const normalized = filename.replace(/[^A-Za-z0-9_-]/g, "_");
  return /^[A-Za-z_]/.test(normalized) ? normalized : `c_${normalized}`;
}

async function projectSourcePath(cwd: string, path: string): Promise<string> {
  const projectRoot = await realpath(cwd);
  const sourcePath = await realpath(resolve(projectRoot, path));
  const projectRelative = relative(projectRoot, sourcePath);
  if (
    projectRelative === ".." ||
    projectRelative.startsWith(`..${sep}`) ||
    isAbsolute(projectRelative)
  ) {
    throw new Error("C source must stay inside the Pi working directory");
  }
  return sourcePath;
}

function resultText(value: unknown): string {
  if (value === undefined) return "undefined";
  if (typeof value === "string" || typeof value === "bigint") return value.toString();
  return JSON.stringify(value);
}

function failure(error: unknown) {
  const message = error instanceof Error ? error.message : String(error);
  return {
    content: [{ type: "text" as const, text: message }],
    details: { error: message },
    isError: true,
  };
}

export default function piCPlugins(pi: ExtensionAPI) {
  const modules = new CModuleRegistry();
  const extensions = new Map<string, BoundPiExtension>();

  const loadedState = () => ({
    modules: modules.list(),
    extensions: [...extensions].map(([sourcePath, extension]) => ({
      sourcePath,
      registrations: extension.registrations.map(({ kind, name }) => ({ kind, name })),
    })),
  });
  const loadedStateText = (state = loadedState()) => {
    return state.modules.length === 0 && state.extensions.length === 0
      ? "No C modules or extensions loaded"
      : JSON.stringify(state, null, 2);
  };

  const loadExtension = async (cwd: string, options: ExtensionInput) => {
    const path = options.path;
    const sourcePath = await projectSourcePath(cwd, path);
    if (extensions.has(sourcePath)) {
      throw new Error(`C extension '${path}' is already loaded`);
    }
    const source = await readFile(sourcePath, "utf8");
    const bound = compilePiExtension(pi, source, {
      library: options.library,
      flags: options.flags,
      define: options.define,
    });
    extensions.set(sourcePath, bound);
    return bound.registrations;
  };

  pi.registerTool({
    name: "c_extension_load",
    label: "Load C Extension",
    description:
      "Compile a C implementation of Pi tools, commands, event hooks, context transforms, and TUI effects",
    parameters: extensionSchema,
    async execute(_toolCallId, params: ExtensionInput, signal, _onUpdate, ctx) {
      try {
        signal?.throwIfAborted();
        const registrations = await loadExtension(ctx.cwd, params);
        return {
          content: [{
            type: "text" as const,
            text: `Loaded C extension: ${registrations.map((item) => `${item.kind}:${item.name}`).join(", ")}`,
          }],
          details: { registrations },
        };
      } catch (error) {
        return failure(error);
      }
    },
  });

  pi.registerTool({
    name: "c_load",
    label: "Load C",
    description:
      "Parse a C file, generate ABI bindings, and compile it in the Pi process with TinyCC",
    parameters: loadSchema,
    async execute(_toolCallId, params: LoadInput, signal, _onUpdate, ctx) {
      try {
        signal?.throwIfAborted();
        const sourcePath = await projectSourcePath(ctx.cwd, params.path);
        const summary = await modules.load(
          params.module ?? defaultModuleId(sourcePath),
          sourcePath,
          { library: params.library, flags: params.flags, define: params.define },
        );
        return {
          content: [
            {
              type: "text" as const,
              text: `Loaded ${summary.id}: ${summary.functions.join(", ")}`,
            },
          ],
          details: summary,
        };
      } catch (error) {
        return failure(error);
      }
    },
  });

  pi.registerTool({
    name: "c_call",
    label: "Call C",
    description: "Call a generated function binding in a loaded TinyCC module",
    parameters: callSchema,
    async execute(_toolCallId, params: CallInput, signal, _onUpdate, ctx) {
      try {
        signal?.throwIfAborted();
        const call = modules.call(params.module, params.function, params.arguments);
        const result = resultText(call.value);
        const text = Object.keys(call.mutableArguments).length === 0
          ? result
          : JSON.stringify({ result, mutableArguments: call.mutableArguments }, null, 2);
        return {
          content: [{ type: "text" as const, text }],
          details: {
            module: params.module,
            function: params.function,
            result,
            mutableArguments: call.mutableArguments,
          },
        };
      } catch (error) {
        return failure(error);
      }
    },
  });

  pi.registerTool({
    name: "c_modules",
    label: "List C Modules",
    description: "List loaded TinyCC modules, C extensions, functions, and registrations",
    parameters: Type.Object({}),
    async execute(_toolCallId, _params, _signal, _onUpdate, _ctx) {
      const state = loadedState();
      return {
        content: [{ type: "text" as const, text: loadedStateText(state) }],
        details: state,
      };
    },
  });

  pi.registerTool({
    name: "c_unload",
    label: "Unload C",
    description: "Close a TinyCC plugin before releasing its compiled code",
    parameters: moduleSchema,
    async execute(_toolCallId, params: ModuleInput, _signal, _onUpdate, _ctx) {
      try {
        modules.unload(params.module);
        return {
          content: [{ type: "text" as const, text: `Unloaded ${params.module}` }],
          details: { module: params.module },
        };
      } catch (error) {
        return failure(error);
      }
    },
  });

  pi.registerCommand("c-extension-load", {
    description: "Compile a C implementation of Pi extension APIs",
    handler: async (args, ctx) => {
      try {
        const registrations = await loadExtension(ctx.cwd, { path: args.trim() });
        ctx.ui.notify(
          `Loaded C extension: ${registrations.map((item) => `${item.kind}:${item.name}`).join(", ")}`,
          "info",
        );
      } catch (error) {
        ctx.ui.notify(error instanceof Error ? error.message : String(error), "error");
      }
    },
  });

  pi.registerCommand("c-modules", {
    description: "List loaded C modules and extensions",
    handler: async (_args, ctx) => {
      ctx.ui.notify(loadedStateText(), "info");
    },
  });

  pi.registerCommand("c-load", {
    description: "Compile a C file with Tree-sitter-generated TinyCC bindings",
    handler: async (args, ctx) => {
      try {
        const sourcePath = await projectSourcePath(ctx.cwd, args.trim());
        const summary = await modules.load(defaultModuleId(sourcePath), sourcePath);
        ctx.ui.notify(`Loaded ${summary.id}: ${summary.functions.join(", ")}`, "info");
      } catch (error) {
        ctx.ui.notify(error instanceof Error ? error.message : String(error), "error");
      }
    },
  });

  pi.on("session_shutdown", async () => {
    for (const extension of extensions.values()) extension.close();
    extensions.clear();
    modules.closeAll();
  });
}
