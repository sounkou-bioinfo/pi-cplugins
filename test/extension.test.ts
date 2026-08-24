import assert from "node:assert/strict";
import { resolve } from "node:path";
import test from "node:test";
import { crc32 } from "node:zlib";

import piCPlugins from "../extension/index.js";

type UI = {
  notify(message: string, level?: string): void;
  setStatus(key: string, text: string | undefined): void;
  setWidget(key: string, lines: string[] | undefined, options?: unknown): void;
};

type Context = { cwd: string; ui: UI };

type Tool = {
  name: string;
  execute: (
    toolCallId: string,
    params: Record<string, unknown>,
    signal: AbortSignal,
    onUpdate: ((value: unknown) => void) | undefined,
    context: Context,
  ) => Promise<{
    content: Array<{ type: string; text: string }>;
    details?: unknown;
    isError?: boolean;
  }>;
};

test("Pi extension executes generated bindings and C-authored Pi APIs", async () => {
  const tools = new Map<string, Tool>();
  const commands = new Map<string, (args: string, context: Context) => Promise<void>>();
  const eventHandlers = new Map<string, Array<(event: unknown, context: Context) => Promise<unknown>>>();
  const fakePi = {
    registerTool(tool: Tool) {
      tools.set(tool.name, tool);
    },
    registerCommand(name: string, command: { handler(args: string, context: Context): Promise<void> }) {
      commands.set(name, command.handler);
    },
    on(event: string, handler: (value: unknown, context: Context) => Promise<unknown>) {
      const handlers = eventHandlers.get(event) ?? [];
      handlers.push(handler);
      eventHandlers.set(event, handlers);
    },
  };
  piCPlugins(fakePi as never);

  assert.deepEqual(
    [...tools.keys()],
    ["c_extension_load", "c_load", "c_call", "c_modules", "c_unload"],
  );
  const uiEvents: unknown[] = [];
  const context: Context = {
    cwd: resolve(import.meta.dirname, ".."),
    ui: {
      notify: (...args) => uiEvents.push(["notify", ...args]),
      setStatus: (...args) => uiEvents.push(["status", ...args]),
      setWidget: (...args) => uiEvents.push(["widget", ...args]),
    },
  };
  const signal = new AbortController().signal;

  const outside = await tools.get("c_load")!.execute(
    "outside",
    { path: "/etc/hosts", module: "outside" },
    signal,
    undefined,
    context,
  );
  assert.equal(outside.isError, true);
  assert.match(outside.content[0].text, /inside the Pi working directory/);

  const loaded = await tools.get("c_load")!.execute(
    "load",
    { path: "examples/ergonomic.c", module: "demo" },
    signal,
    undefined,
    context,
  );
  assert.equal(loaded.isError, undefined);
  assert.match(loaded.content[0].text, /Loaded demo/);

  const called = await tools.get("c_call")!.execute(
    "call",
    { module: "demo", function: "add", arguments: [19, 23] },
    signal,
    undefined,
    context,
  );
  assert.equal(called.isError, undefined);
  assert.equal(called.content[0].text, "42");

  await tools.get("c_load")!.execute(
    "ffi-load",
    { path: "examples/ffi.c", module: "ffi-agent" },
    signal,
    undefined,
    context,
  );
  const mutated = await tools.get("c_call")!.execute(
    "ffi-reverse",
    { module: "ffi-agent", function: "reverse4", arguments: [[1, 2, 3, 4]] },
    signal,
    undefined,
    context,
  );
  assert.deepEqual(JSON.parse(mutated.content[0].text), {
    result: "undefined",
    mutableArguments: { values: [4, 3, 2, 1] },
  });

  const linked = await tools.get("c_load")!.execute(
    "zlib-load",
    { path: "examples/zlib.c", module: "zlib", library: "z" },
    signal,
    undefined,
    context,
  );
  assert.equal(linked.isError, undefined);
  const version = await tools.get("c_call")!.execute(
    "zlib-version",
    { module: "zlib", function: "linked_zlib_version", arguments: [] },
    signal,
    undefined,
    context,
  );
  assert.match(version.content[0].text, /^\d+\.\d+/);
  const bytes = [...Buffer.from("Pi agents execute real C libraries")];
  const checksum = await tools.get("c_call")!.execute(
    "zlib-crc",
    { module: "zlib", function: "linked_zlib_crc32", arguments: [bytes, bytes.length] },
    signal,
    undefined,
    context,
  );
  assert.equal(Number(checksum.content[0].text), crc32(Buffer.from(bytes)));

  const extension = await tools.get("c_extension_load")!.execute(
    "extension",
    { path: "examples/pi_extension.c" },
    signal,
    undefined,
    context,
  );
  assert.equal(extension.isError, undefined);
  assert.match(extension.content[0].text, /tool:c_extension_demo/);
  assert.ok(commands.has("c-extension-demo"));

  const updates: unknown[] = [];
  const cTool = await tools.get("c_extension_demo")!.execute(
    "c-tool",
    { name: "Pi" },
    signal,
    (value) => updates.push(value),
    context,
  );
  assert.match(cTool.content[0].text, /Hello from a C-authored Pi extension/);
  assert.equal(updates.length, 1);
  assert.deepEqual(uiEvents.map((event) => (event as unknown[])[0]), [
    "notify", "status", "widget",
  ]);

  const beforeHandlers = eventHandlers.get("before_agent_start") ?? [];
  assert.equal(beforeHandlers.length, 1);
  assert.deepEqual(await beforeHandlers[0]({ systemPrompt: "base prompt" }, context), {
    systemPrompt: "This system prompt was returned directly by a TinyCC C callback.",
  });

  const contextHandlers = eventHandlers.get("context") ?? [];
  assert.equal(contextHandlers.length, 1);
  assert.deepEqual(await contextHandlers[0]({ messages: [{ role: "user", content: "original" }] }, context), {
    messages: [{
      role: "user",
      content: "This provider context was returned directly by a C hook.",
    }],
  });

  const resultHandlers = eventHandlers.get("tool_result") ?? [];
  assert.equal(resultHandlers.length, 1);
  const transformed = await resultHandlers[0]({ toolName: "c_extension_demo" }, context);
  assert.deepEqual(transformed, {
    content: [{ type: "text", text: "C tool result returned directly by a C tool_result hook" }],
    details: { transformedBy: "examples/pi_extension.c" },
    isError: false,
  });

  const zlibExtension = await tools.get("c_extension_load")!.execute(
    "zlib-extension",
    { path: "examples/zlib_extension.c", library: "z" },
    signal,
    undefined,
    context,
  );
  assert.equal(zlibExtension.isError, undefined);
  assert.match(zlibExtension.content[0].text, /tool:c_zlib_crc32/);
  assert.ok(commands.has("c-zlib-version"));

  const zlibUpdates: unknown[] = [];
  const zlibTool = await tools.get("c_zlib_crc32")!.execute(
    "zlib-tool",
    { text: "Pi extension ABI" },
    signal,
    (value) => zlibUpdates.push(value),
    context,
  );
  assert.equal(zlibUpdates.length, 1);
  assert.match(zlibTool.content[0].text, /zlib \d+\.\d+.*CRC32 4273009869 for 16 bytes/);
  assert.deepEqual(zlibTool.details, {
    implementation: "examples/zlib_extension.c",
    library: "zlib",
    version: (zlibTool.details as { version: string }).version,
    crc32: 4273009869,
    bytes: 16,
  });

  const allResultHandlers = eventHandlers.get("tool_result") ?? [];
  assert.equal(allResultHandlers.length, 2);
  const verified = await allResultHandlers[1]({
    toolName: "c_zlib_crc32",
    content: zlibTool.content,
    details: zlibTool.details,
  }, context);
  assert.deepEqual(verified, {
    content: [
      ...zlibTool.content,
      { type: "text", text: "C tool_result hook verified the zlib result." },
    ],
    details: {
      ...(zlibTool.details as Record<string, unknown>),
      verifiedBy: "examples/zlib_extension.c",
    },
    isError: false,
  });

  const listed = await tools.get("c_modules")!.execute(
    "list-all",
    {},
    signal,
    undefined,
    context,
  );
  const loadedState = JSON.parse(listed.content[0].text) as {
    modules: Array<{ id: string }>;
    extensions: Array<{
      sourcePath: string;
      registrations: Array<{ kind: string; name: string }>;
    }>;
  };
  assert.ok(loadedState.modules.some(({ id }) => id === "ffi-agent"));
  assert.ok(loadedState.extensions.some(({ sourcePath, registrations }) =>
    sourcePath.endsWith("examples/zlib_extension.c") &&
    registrations.some(({ kind, name }) => kind === "tool" && name === "c_zlib_crc32")
  ));

  assert.ok(commands.has("c-modules"));
  await commands.get("c-modules")!("", context);
  assert.match(String((uiEvents.at(-1) as unknown[])[1]), /c_zlib_crc32/);

  for (const handler of eventHandlers.get("session_shutdown") ?? []) {
    await handler({}, context);
  }
  const empty = await tools.get("c_modules")!.execute(
    "list-empty",
    {},
    signal,
    undefined,
    context,
  );
  assert.equal(empty.content[0].text, "No C modules or extensions loaded");
});
