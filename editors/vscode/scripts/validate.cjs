"use strict";

const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");

function fail(message) {
  throw new Error(message);
}

function readJson(relativePath) {
  const filename = path.join(root, relativePath);
  return JSON.parse(fs.readFileSync(filename, "utf8"));
}

function requireFile(relativePath) {
  if (!fs.existsSync(path.join(root, relativePath))) {
    fail(`missing contributed file: ${relativePath}`);
  }
}

function validatePatterns(node, location = "grammar") {
  if (Array.isArray(node)) {
    node.forEach((value, index) =>
      validatePatterns(value, `${location}[${index}]`));
    return;
  }
  if (node === null || typeof node !== "object") {
    return;
  }

  for (const property of ["match", "begin", "end"]) {
    if (typeof node[property] === "string") {
      try {
        new RegExp(node[property]);
      } catch (error) {
        fail(`${location}.${property} is not a valid regex: ${error.message}`);
      }
    }
  }
  for (const [key, value] of Object.entries(node)) {
    validatePatterns(value, `${location}.${key}`);
  }
}

const manifest = readJson("package.json");
const language = manifest.contributes?.languages?.find(
  (candidate) => candidate.id === "concept");
if (!language || !language.extensions?.includes(".concept")) {
  fail("manifest must register .concept files as the Concept language");
}
requireFile(language.configuration);

const grammarContribution = manifest.contributes?.grammars?.find(
  (candidate) => candidate.language === "concept");
if (!grammarContribution || grammarContribution.scopeName !== "source.concept") {
  fail("manifest must contribute the source.concept TextMate grammar");
}
requireFile(grammarContribution.path);

const snippetContribution = manifest.contributes?.snippets?.find(
  (candidate) => candidate.language === "concept");
if (!snippetContribution) {
  fail("manifest must contribute Concept snippets");
}
requireFile(snippetContribution.path);

const languageConfiguration = readJson(language.configuration);
if (languageConfiguration.comments?.lineComment !== "//") {
  fail("Concept line comments must use //");
}
if ("blockComment" in (languageConfiguration.comments ?? {})) {
  fail("Concept does not support block comments");
}

const grammar = readJson(grammarContribution.path);
validatePatterns(grammar);
const grammarSource = JSON.stringify(grammar);
for (const token of [
  "import", "class", "constructor", "fn", "return", "if", "else", "while",
  "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
  "f32", "f64", "int", "float", "double", "string", "text", "void",
  "true", "false", "this", "complexity", "ptr_cast", "malloc", "free"
]) {
  if (!grammarSource.includes(token)) {
    fail(`grammar does not cover required token: ${token}`);
  }
}

const snippets = readJson(snippetContribution.path);
for (const [name, snippet] of Object.entries(snippets)) {
  if (typeof snippet.prefix !== "string" || snippet.prefix.length === 0) {
    fail(`snippet '${name}' has no prefix`);
  }
  if (!Array.isArray(snippet.body) && typeof snippet.body !== "string") {
    fail(`snippet '${name}' has no body`);
  }
}

console.log(
  `Validated Concept manifest, language configuration, grammar, and ` +
  `${Object.keys(snippets).length} snippets.`);
