"use strict";

const fs = require("node:fs");
const path = require("node:path");
const textmate = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");

async function main() {
  const wasm = fs.readFileSync(
    require.resolve("vscode-oniguruma/release/onig.wasm"));
  await oniguruma.loadWASM(wasm.buffer.slice(
    wasm.byteOffset, wasm.byteOffset + wasm.byteLength));

  const registry = new textmate.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
      createOnigString: (value) => new oniguruma.OnigString(value)
    }),
    loadGrammar: async (scopeName) => {
      if (scopeName !== "source.concept") {
        return null;
      }
      const filename = path.resolve(
        __dirname, "../syntaxes/concept.tmLanguage.json");
      return textmate.parseRawGrammar(
        fs.readFileSync(filename, "utf8"), filename);
    }
  });

  const grammar = await registry.loadGrammar("source.concept");
  if (!grammar) {
    throw new Error("Concept grammar did not load");
  }

  const expectedScopes = new Map([
    ["class", "storage.type.class.concept"],
    ["std::array", "entity.name.type.class.concept"],
    ["T", "entity.name.type.parameter.concept"],
    ["u64", "storage.type.primitive.concept"],
    ["fn", "storage.type.function.concept"],
    ["grow", "entity.name.function.concept"],
    ["if", "keyword.control.concept"],
    ["this", "variable.language.this.concept"],
    ["return", "keyword.control.concept"],
    ["true", "constant.language.boolean.concept"]
  ]);
  const observedScopes = new Map();
  let ruleStack = textmate.INITIAL;
  for (const line of [
    "class std::array<T> {",
    "    u64 length;",
    "    fn grow() -> bool {",
    "        if (this.capacity != 0) {",
    "            return true;",
    "        }",
    "    }",
    "}"
  ]) {
    const result = grammar.tokenizeLine(line, ruleStack);
    ruleStack = result.ruleStack;
    for (const token of result.tokens) {
      const value = line.slice(token.startIndex, token.endIndex).trim();
      if (expectedScopes.has(value)) {
        observedScopes.set(value, token.scopes);
      }
    }
  }

  for (const [token, expectedScope] of expectedScopes) {
    const scopes = observedScopes.get(token);
    if (!scopes?.includes(expectedScope)) {
      throw new Error(
        `${JSON.stringify(token)} is missing TextMate scope ${expectedScope}; ` +
        `observed ${scopes?.join(" ") ?? "no token"}`);
    }
  }
  console.log(
    `Loaded the grammar with VS Code's TextMate/Oniguruma engine and ` +
    `verified ${expectedScopes.size} representative token scopes.`);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
