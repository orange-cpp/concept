# Concept Language Support

VS Code language support for `.concept` source files.

## Features

- TextMate highlighting for declarations, control flow, core types, literals,
  strings and escapes, decorators, pointers, operators, built-ins, generics,
  qualified `std::` names, and function calls
- `//` comment toggling
- Bracket matching, automatic closing, indentation, and region folding
- Snippets for imports, functions, entry points, classes, constructors,
  control flow, arrays, heap allocation, and native pointer casts

The extension follows the syntax implemented by the compiler. It intentionally
does not enable block comments or unsupported language constructs.

This version is declarative and does not yet provide semantic diagnostics,
completion, navigation, formatting, or debugging. Those features require a
Concept language server or compiler analysis API.

## Install

Install the packaged VSIX from the repository root:

```powershell
code --install-extension .\dist\concept-language-support-0.1.1.vsix --force
```

Alternatively, run **Extensions: Install from VSIX...** from the VS Code
Command Palette and select the same file.

## Develop and package

```powershell
cd editors\vscode
npm test
npm run package:vsix
```

The packaging command writes
`dist/concept-language-support-0.1.1.vsix` at the repository root. The
`publisher` value in `package.json` is a local placeholder and must be replaced
with an owned Visual Studio Marketplace publisher ID before publication.
