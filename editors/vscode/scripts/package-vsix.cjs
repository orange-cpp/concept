"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const extensionRoot = path.resolve(__dirname, "..");
const manifest = JSON.parse(
  fs.readFileSync(path.join(extensionRoot, "package.json"), "utf8"));
const outputDirectory = path.resolve(extensionRoot, "../../dist");
const output = path.join(
  outputDirectory,
  `${manifest.name}-${manifest.version}.vsix`);

fs.mkdirSync(outputDirectory, { recursive: true });

const vscePackage = require.resolve("@vscode/vsce/package.json");
const vsceManifest = JSON.parse(fs.readFileSync(vscePackage, "utf8"));
const vsceCli = path.resolve(path.dirname(vscePackage), vsceManifest.bin.vsce);
const result = spawnSync(
  process.execPath,
  [vsceCli, "package", "--allow-missing-repository", "--out", output],
  {
    cwd: extensionRoot,
    stdio: "inherit"
  });

if (result.error) {
  throw result.error;
}
if (result.status !== 0) {
  process.exitCode = result.status ?? 1;
}
