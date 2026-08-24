import {
  compileNativePlugin,
  publicIncludePath,
  type NativeCompileOptions,
  type NativeExtensionBridge,
  type NativeExtensionRegistration,
  type NativePlugin,
} from "./native.js";

interface PiUI {
  notify(message: string, level?: "info" | "warning" | "error"): void;
  setStatus(key: string, text: string | undefined): void;
  setWidget(
    key: string,
    value: string[] | undefined,
    options?: { placement: "aboveEditor" | "belowEditor" },
  ): void;
}

interface PiContext {
  cwd: string;
  ui: PiUI;
}

interface PiTool {
  name: string;
  label: string;
  description: string;
  parameters: unknown;
  execute(
    toolCallId: string,
    parameters: unknown,
    signal: AbortSignal,
    onUpdate: ((value: unknown) => void) | undefined,
    context: PiContext,
  ): Promise<unknown>;
}

interface PiCommand {
  description: string;
  handler(args: string, context: PiContext): Promise<void>;
}

export interface PiExtensionAPI {
  registerTool(tool: PiTool): void;
  registerCommand(name: string, command: PiCommand): void;
  on(eventName: string, handler: (event: unknown, context: PiContext) => Promise<unknown>): void;
}

export interface BoundPiExtension {
  readonly registrations: readonly NativeExtensionRegistration[];
  close(): void;
}

function parseJson(text: string | undefined): unknown {
  return text === undefined ? undefined : JSON.parse(text);
}

function json(value: unknown): string {
  return JSON.stringify(value, (_key, item) =>
    typeof item === "bigint" ? item.toString() : item,
  );
}

function bridgeFor(
  context: PiContext,
  signal?: AbortSignal,
  onUpdate?: (value: unknown) => void,
): NativeExtensionBridge {
  return {
    isCancelled: () => signal?.aborted ?? false,
    update: (value) => onUpdate?.(parseJson(value)),
    notify: (message, level) => {
      const levels = ["info", "warning", "error"] as const;
      context.ui.notify(message, levels[level] ?? "info");
    },
    setStatus: (key, text) => context.ui.setStatus(key, text ?? undefined),
    setWidget: (key, linesJson, placement) => {
      const lines = linesJson === null ? undefined : parseJson(linesJson);
      if (lines !== undefined &&
          (!Array.isArray(lines) || lines.some((line) => typeof line !== "string"))) {
        throw new Error("C extension widget must be a JSON string array");
      }
      context.ui.setWidget(
        key,
        lines as string[] | undefined,
        { placement: placement === 1 ? "belowEditor" : "aboveEditor" },
      );
    },
  };
}

function invoke(
  plugin: NativePlugin,
  registration: NativeExtensionRegistration,
  input: unknown,
  context: PiContext,
  signal?: AbortSignal,
  onUpdate?: (value: unknown) => void,
): unknown {
  const output = plugin.invoke(
    registration.id,
    json(input),
    context.cwd,
    bridgeFor(context, signal, onUpdate),
  );
  return parseJson(output);
}

export function compilePiExtension(
  pi: PiExtensionAPI,
  source: string,
  options: NativeCompileOptions = {},
): BoundPiExtension {
  const plugin = compileNativePlugin(source, [publicIncludePath()], options);
  try {
    return bindPiExtension(pi, plugin);
  } catch (error) {
    plugin.close();
    throw error;
  }
}

export function bindPiExtension(
  pi: PiExtensionAPI,
  plugin: NativePlugin,
): BoundPiExtension {
  const registrations = plugin.registrations();
  for (const registration of registrations) {
    if (registration.kind === "tool") {
      pi.registerTool({
        name: registration.name,
        label: registration.label!,
        description: registration.description!,
        parameters: JSON.parse(registration.parametersJson!) as never,
        async execute(_toolCallId, parameters, signal, onUpdate, context) {
          return invoke(
            plugin,
            registration,
            parameters,
            context,
            signal,
            onUpdate as (value: unknown) => void,
          ) as never;
        },
      });
      continue;
    }
    if (registration.kind === "command") {
      pi.registerCommand(registration.name, {
        description: registration.description ?? "C extension command",
        handler: async (args, context) => {
          invoke(plugin, registration, { args }, context);
        },
      });
      continue;
    }
    const on = pi.on as unknown as (
      eventName: string,
      handler: (event: unknown, context: PiContext) => Promise<unknown>,
    ) => void;
    on(registration.name, async (event, context) =>
      invoke(plugin, registration, event, context),
    );
  }
  return {
    registrations,
    close: () => plugin.close(),
  };
}
