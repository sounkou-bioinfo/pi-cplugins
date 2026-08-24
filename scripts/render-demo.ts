import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

import piCPlugins from "../extension/index.js";

type ToolResult = {
  content: Array<{ type: string; text: string }>;
  details?: {
    mutableArguments?: Record<string, Array<number | string>>;
  };
  isError?: boolean;
};

type Tool = {
  name: string;
  execute: (
    toolCallId: string,
    params: Record<string, unknown>,
    signal: AbortSignal,
    onUpdate: undefined,
    context: { cwd: string },
  ) => Promise<ToolResult>;
};

const root = resolve(import.meta.dirname, "..");
const outputDirectory = resolve(root, "examples/rendered");
const svgPath = resolve(outputDirectory, "pi-cplugins.svg");
const tools = new Map<string, Tool>();
const shutdownHandlers: Array<() => Promise<void>> = [];

piCPlugins({
  registerTool(tool: Tool) {
    tools.set(tool.name, tool);
  },
  registerCommand() {},
  on(event: string, handler: () => Promise<void>) {
    if (event === "session_shutdown") shutdownHandlers.push(handler);
  },
} as never);

const context = { cwd: root };
const signal = new AbortController().signal;

async function callTool(name: string, params: Record<string, unknown>): Promise<ToolResult> {
  const tool = tools.get(name);
  if (!tool) throw new Error(`Pi extension did not register ${name}`);
  const result = await tool.execute(name, params, signal, undefined, context);
  if (result.isError) throw new Error(result.content[0]?.text ?? `${name} failed`);
  return result;
}

async function assertArtifact(path: string, expected: string): Promise<void> {
  let actual: string;
  try {
    actual = await readFile(path, "utf8");
  } catch {
    throw new Error(`${path} is missing; run npm run demo`);
  }
  if (actual !== expected) throw new Error(`${path} drifted; run npm run demo`);
}

try {
  const loaded = await callTool("c_load", {
    path: "examples/render.c",
    module: "render",
  });
  const pointerModule = await callTool("c_load", {
    path: "examples/ffi.c",
    module: "ffi",
  });
  const reversed = await callTool("c_call", {
    module: "ffi",
    function: "reverse4",
    arguments: [[1, 2, 3, 4]],
  });
  const width = await callTool("c_call", {
    module: "render",
    function: "render_width",
    arguments: [],
  });
  const height = await callTool("c_call", {
    module: "render",
    function: "render_height",
    arguments: [],
  });
  const rendered = await callTool("c_call", {
    module: "render",
    function: "render_card",
    arguments: [188],
  });

  const svg = `${rendered.content[0]?.text ?? ""}\n`;
  const transcript = [
    "$ npm run demo",
    `c_load => ${loaded.content[0]?.text}`,
    `c_load => ${pointerModule.content[0]?.text}`,
    `c_call reverse4 [[1,2,3,4]] => mutable values ${JSON.stringify(
      reversed.details?.mutableArguments?.values,
    )}`,
    `c_call render_width [] => ${width.content[0]?.text}`,
    `c_call render_height [] => ${height.content[0]?.text}`,
    `c_call render_card [188] => examples/rendered/pi-cplugins.svg (${Buffer.byteLength(svg)} bytes)`,
    "session_shutdown => plugin close, then tcc_delete",
    "",
  ].join("\n");

  if (process.argv.includes("--check")) {
    await assertArtifact(svgPath, svg);
  } else {
    await mkdir(outputDirectory, { recursive: true });
    await writeFile(svgPath, svg);
  }
  process.stdout.write(transcript);
} finally {
  await Promise.all(shutdownHandlers.map((handler) => handler()));
}
