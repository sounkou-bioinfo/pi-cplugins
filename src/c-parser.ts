import { createRequire } from "node:module";

import { Language, Parser, type Node as SyntaxNode } from "web-tree-sitter";

import type { FFIType } from "./native.js";

export type BindingKind = FFIType;

export interface CallbackParameter {
  cType: string;
  kind: Exclude<BindingKind, "void" | "function" | "napi_env" | "napi_value">;
}

export interface CallbackSignature {
  returnCType: string;
  returnKind: Exclude<BindingKind, "function" | "napi_env" | "napi_value">;
  parameters: CallbackParameter[];
}

export interface BindingParameter {
  name: string;
  cType: string;
  kind: Exclude<BindingKind, "void">;
  callback?: CallbackSignature;
  pointee?: string;
  mutable?: boolean;
}

export interface BindingFunction {
  id: number;
  name: string;
  returnCType: string;
  returnKind: BindingKind;
  parameters: BindingParameter[];
}

const require = createRequire(import.meta.url);
const cWasmPath = require.resolve("tree-sitter-c/tree-sitter-c.wasm");

let parserPromise: Promise<Parser> | undefined;

async function cParser(): Promise<Parser> {
  if (!parserPromise) {
    parserPromise = (async () => {
      await Parser.init();
      const parser = new Parser();
      parser.setLanguage(await Language.load(cWasmPath));
      return parser;
    })();
  }
  return parserPromise;
}

function findDescendant(node: SyntaxNode, type: string): SyntaxNode | undefined {
  if (node.type === type) return node;
  for (const child of node.namedChildren) {
    if (child) {
      const found = findDescendant(child, type);
      if (found) return found;
    }
  }
  return undefined;
}

function withoutComments(value: string): string {
  return value
    .replace(/\/\*[\s\S]*?\*\//g, " ")
    .replace(/\/\/[^\n]*/g, " ");
}

function normalizeType(value: string): string {
  return withoutComments(value)
    .replace(/\b(?:static|extern|inline|register|auto|volatile|restrict)\b/g, " ")
    .replace(/\s+/g, " ")
    .replace(/\s*\*\s*/g, "*")
    .trim();
}

function bindingKind(cType: string, allowVoid: boolean): BindingKind | undefined {
  const normalized = normalizeType(cType).replace(/^unsigned$/, "unsigned int");
  const unqualified = normalized
    .replace(/^const\s+/, "")
    .replace(/\s+const(?=\*)/, "")
    .replace(/^signed\s+/, "");

  if (allowVoid && unqualified === "void") return "void";
  const composite = /^(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)$/.exec(unqualified);
  if (composite) return `${composite[1]}:${composite[2]}` as BindingKind;
  if (unqualified.endsWith("*")) {
    const pointee = unqualified.slice(0, -1).trim();
    if (pointee === "char") return /\bconst\b/.test(normalized) ? "cstring" : "buffer";
    return "ptr";
  }
  if (unqualified === "bool" || unqualified === "_Bool") return "bool";
  if (unqualified === "char") return "char";
  if (unqualified === "int8_t" || unqualified === "signed char") return "i8";
  if (unqualified === "uint8_t" || unqualified === "unsigned char") return "u8";
  if (unqualified === "int16_t" || unqualified === "short" || unqualified === "short int") return "i16";
  if (unqualified === "uint16_t" || unqualified === "unsigned short" || unqualified === "unsigned short int") return "u16";
  if (unqualified === "int32_t" || unqualified === "int") return "i32";
  if (unqualified === "uint32_t" || unqualified === "unsigned int") return "u32";
  if (unqualified === "int64_t") return "i64";
  if (unqualified === "uint64_t" || unqualified === "size_t") return "u64";
  if (unqualified === "float") return "f32";
  if (unqualified === "double") return "f64";
  if (unqualified === "napi_env") return "napi_env";
  if (unqualified === "napi_value") return "napi_value";
  return undefined;
}

function functionName(declarator: SyntaxNode): SyntaxNode | undefined {
  const functionDeclarator = findDescendant(declarator, "function_declarator");
  const named = functionDeclarator?.childForFieldName("declarator");
  return named ? findDescendant(named, "identifier") : undefined;
}

function callbackParameter(source: string, node: SyntaxNode): CallbackParameter | "void" {
  const declarator = node.childForFieldName("declarator");
  const identifier = declarator ? findDescendant(declarator, "identifier") : undefined;
  const cType = normalizeType(identifier
    ? source.slice(node.startIndex, identifier.startIndex)
    : source.slice(node.startIndex, node.endIndex));
  const kind = bindingKind(cType, true);
  if (!kind || kind === "function" || kind === "napi_env" || kind === "napi_value") {
    throw new Error(`callback parameter has unsupported type '${cType}'`);
  }
  if (kind === "void") return "void";
  return { cType, kind };
}

function callbackSignature(source: string, node: SyntaxNode,
                           declarator: SyntaxNode): CallbackSignature {
  const functionDeclarator = findDescendant(declarator, "function_declarator");
  const parameterList = functionDeclarator?.childForFieldName("parameters");
  if (!functionDeclarator || !parameterList) {
    throw new Error("callback declarator has no parameter list");
  }
  if (parameterList.namedChildren.some((child) => child?.type === "variadic_parameter")) {
    throw new Error("variadic callbacks are unsupported");
  }
  const returnCType = normalizeType(source.slice(node.startIndex, declarator.startIndex));
  const returnKind = bindingKind(returnCType, true);
  if (!returnKind || returnKind === "function" || returnKind === "napi_env" ||
      returnKind === "napi_value" || returnKind === "cstring") {
    throw new Error(`callback has unsupported return type '${returnCType}'`);
  }
  const parameters: CallbackParameter[] = [];
  for (const child of parameterList.namedChildren) {
    if (!child || child.type !== "parameter_declaration") {
      throw new Error(`callback has unsupported parameter syntax '${child?.type}'`);
    }
    const parameter = callbackParameter(source, child);
    if (parameter === "void") {
      if (parameterList.namedChildren.length !== 1) {
        throw new Error("void must be the callback's only parameter");
      }
    } else {
      parameters.push(parameter);
    }
  }
  return { returnCType, returnKind, parameters };
}

function parameterBinding(source: string, node: SyntaxNode): BindingParameter | "void" {
  const declarator = node.childForFieldName("declarator");
  if (!declarator) {
    const type = normalizeType(source.slice(node.startIndex, node.endIndex));
    if (type === "void") return "void";
    throw new Error(`parameter without a name has unsupported type '${type}'`);
  }

  const nameNode = findDescendant(declarator, "identifier");
  if (!nameNode) throw new Error("parameter declarator has no identifier");
  const cType = normalizeType(source.slice(node.startIndex, nameNode.startIndex));
  let kind = bindingKind(cType, false);
  if (findDescendant(declarator, "array_declarator")) kind = "ptr";
  const functionDeclarator = findDescendant(declarator, "function_declarator");
  if (functionDeclarator) kind = "function";
  if (!kind || kind === "void") {
    throw new Error(`parameter '${nameNode.text}' has unsupported type '${cType}'`);
  }
  const pointee = kind === "ptr" || kind === "buffer" || kind === "cstring"
    ? cType.replace(/\bconst\b/g, "").replace(/\*/g, "").trim()
    : undefined;
  return {
    name: nameNode.text,
    cType,
    kind,
    callback: functionDeclarator ? callbackSignature(source, node, declarator) : undefined,
    pointee,
    mutable: kind === "buffer" || (kind === "ptr" && !/\bconst\b/.test(cType)),
  };
}

export async function parseBindings(source: string): Promise<BindingFunction[]> {
  const parser = await cParser();
  const tree = parser.parse(source);
  if (!tree) throw new Error("Tree-sitter did not return a C syntax tree");
  if (tree.rootNode.hasError) {
    const errorNode = findDescendant(tree.rootNode, "ERROR");
    const location = errorNode?.startPosition ?? tree.rootNode.startPosition;
    throw new Error(`C parse error at ${location.row + 1}:${location.column + 1}`);
  }

  const definitions = tree.rootNode
    .descendantsOfType("function_definition")
    .filter((node): node is SyntaxNode => node !== null);
  const functions: Omit<BindingFunction, "id">[] = [];
  const failures: string[] = [];

  for (const definition of definitions) {
    const isStatic = definition.namedChildren.some(
      (child) => child?.type === "storage_class_specifier" && child.text === "static",
    );
    if (isStatic) continue;

    const declarator = definition.childForFieldName("declarator");
    const nameNode = declarator ? functionName(declarator) : undefined;
    if (!declarator || !nameNode) {
      failures.push(`line ${definition.startPosition.row + 1}: cannot identify function declarator`);
      continue;
    }
    if (nameNode.text === "main") continue;
    if (nameNode.text === "pi_plugin_init") {
      failures.push(
        `line ${definition.startPosition.row + 1}: pi_plugin_init is already a plugin entrypoint, not an auto-binding target`,
      );
      continue;
    }

    try {
      const returnCType = normalizeType(source.slice(definition.startIndex, nameNode.startIndex));
      const returnKind = bindingKind(returnCType, true);
      if (!returnKind) throw new Error(`unsupported return type '${returnCType}'`);

      const functionDeclarator = findDescendant(declarator, "function_declarator");
      const parameterList = functionDeclarator?.childForFieldName("parameters");
      if (!parameterList) throw new Error("function has no parameter list");
      const parameterNodes = parameterList.namedChildren.filter(
        (child): child is SyntaxNode => child !== null,
      );
      if (parameterNodes.some((child) => child.type === "variadic_parameter")) {
        throw new Error("variadic functions are unsupported");
      }
      const unsupportedParameter = parameterNodes.find(
        (child) => child.type !== "parameter_declaration",
      );
      if (unsupportedParameter) {
        throw new Error(`unsupported parameter syntax '${unsupportedParameter.type}'`);
      }

      const parameters: BindingParameter[] = [];
      for (const parameter of parameterNodes) {
        const binding = parameterBinding(source, parameter);
        if (binding === "void") {
          if (parameterList.namedChildren.length !== 1) {
            throw new Error("void must be the only parameter");
          }
        } else {
          parameters.push(binding);
        }
      }
      functions.push({
        name: nameNode.text,
        returnCType,
        returnKind,
        parameters,
      });
    } catch (error) {
      failures.push(
        `line ${definition.startPosition.row + 1}, ${nameNode.text}: ${(error as Error).message}`,
      );
    }
  }

  if (failures.length > 0) {
    throw new Error(`cannot generate C bindings:\n${failures.join("\n")}`);
  }
  if (functions.length === 0) {
    throw new Error("C source has no non-static functions to bind");
  }
  return functions.map((fn, id) => ({ id, ...fn }));
}
