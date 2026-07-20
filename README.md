# Concept

Concept is an experimental C-like language designed to help protect compiled applications from reverse engineering. It compiles programs to bytecode and packages each one as a native executable containing both the Concept virtual machine and the program bytecode.


The implementation uses C++23 and currently targets a small bootstrap language.

## Build

With Visual Studio 2022 or another C++23 compiler:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

MSVC builds use the static CRT: `/MT` in Release and `/MTd` in Debug.

## Compile a Concept program

```powershell
.\build\Release\concept.exe .\examples\answer.concept -o answer.exe
.\answer.exe
$LASTEXITCODE # 42
```

For a single-config generator, the compiler is normally at `build/concept.exe`
instead. The compiler locates `concept-runtime.exe` beside itself; use
`--runtime <path>` to select it explicitly.

The interactive calculator example can be built and run with:

```powershell
.\build\Release\concept.exe .\examples\calculator.concept -o calculator.exe
.\calculator.exe
```

## Current syntax

```c
fn calculate() -> double {
    float factor = 1.5;
    return f64(factor) * 28.0;
}

fn main() -> int {
    u8 wrapping = 255;
    f64 answer = calculate();
    bool valid = answer == 42.0;

    wrapping = wrapping + u8(1);
    if (valid) {
        if (wrapping == u8(0)) {
            return i32(answer);
        }
    }
    return 0;
}
```

The bootstrap subset supports:

- `bool`, signed `i8`/`i16`/`i32`/`i64`, unsigned
  `u8`/`u16`/`u32`/`u64`, and floating `f32`/`f64` values;
- aliases `int` = `i32`, `float` = `f32`, and `double` = `f64`;
- owned `string` values and escaped string literals;
- integer, floating-point, string, `true`, and `false` literals;
- explicit casts such as `u8(value)`, `f64(value)`, and `bool(value)`;
- no-argument typed functions, including forward calls and recursion;
- typed local variables, assignment, blocks, `if`/`else`, and `while`;
- `+`, `-`, `*`, `/`, `%`, unary `-` and `!`;
- `==`, `!=`, `<`, `<=`, `>`, and `>=`;
- `input()`/`input_text()` for whole lines, plus `input_i64()` and
  `input_f64()` for parsed numeric lines;
- `print(value)` and `println(value)` for every core value type;
- `//` line comments.

Strings currently support storage, function returns, printing, and `==`/`!=`.
Concatenation, indexing, and conversion between strings and numeric values are
not implemented yet.

Every program must define `main` with an integral or `bool` return type. Its
result becomes the executable's process exit code. Numeric values are converted
as needed by initialization, assignment, return statements, mixed arithmetic,
and conditions. Narrow integral arithmetic wraps at the operation's selected
width. Integral division and modulo by zero are runtime errors; floating-point
operations use IEEE behavior. Floating-to-integral casts reject non-finite and
out-of-range values at runtime.

## Architecture

The compiler performs this pipeline:

```text
source -> lexer/parser -> bytecode compiler -> bytecode image
       -> copy native VM stub -> append image and trailer -> program.exe
```

The runtime reads the trailer from its own executable, verifies and deserializes
the bytecode, and executes it on a stack VM. The bytecode image is versioned so
the instruction set can evolve deliberately.

Every compiler invocation generates a fresh opcode-layout seed. That seed
shuffles the VM's instruction bytes to unique values across the full `0..255`
range, so compiling identical source twice produces different bytecode. The
runtime reconstructs the inverse mapping before validation and execution. This
is an obfuscation layer, not encryption: the executable necessarily contains
enough information for its VM to recover the mapping.

Self-hosting is a later bootstrap stage: extend this subset until a Concept
compiler can be written in Concept, compile it with the C++ compiler, then package
that compiler bytecode with the same native VM. The native VM remains the small
trusted runtime included in generated executables.
