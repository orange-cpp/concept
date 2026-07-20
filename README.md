# Concept

Concept is an experimental C-like language designed to help protect compiled applications from reverse engineering. It compiles programs to bytecode and packages each one as a native executable or Windows DLL containing both the Concept virtual machine and the program bytecode.

See the [Concept language syntax reference](docs/SYNTAX.md) for the grammar,
types, operators, pointers, classes, imports, and built-in APIs implemented now.

The implementation uses C++23 and currently targets a small bootstrap language.

## Build

With Visual Studio 2022 or another C++23 compiler:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

MSVC builds use the static CRT: `/MT` in Release and `/MTd` in Debug.
The first CMake configure downloads a SHA-256-verified, pinned revision of
[JustasMasiulis/xorstr](https://github.com/JustasMasiulis/xorstr) for native
runtime string obfuscation.

## GitHub Actions

`Windows CI` builds the x64 MSVC Release configuration and runs the complete
CTest suite for every push, pull request, and manual run.

`Windows release` builds and tests a release commit, stages a self-contained
Windows x64 distribution, and attaches a versioned ZIP to the matching GitHub
release. The archive contains `concept.exe`, both runtime binaries, the
`concept/std` library sources, the syntax reference, README, and third-party
notices.

[GitHub does not trigger release workflows for draft-release creation](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release).
To attach the archive while the release is still a draft:

1. Save the draft release in GitHub.
2. Open **Actions**, select **Windows release**, and choose **Run workflow**.
3. Enter the draft's exact release tag in `release_tag`.

Publishing a release also runs this workflow automatically and replaces an
existing archive with a freshly tested build. No additional repository secret
is required; the workflow grants the built-in `GITHUB_TOKEN` only the repository
contents permission needed to upload the asset.

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

On Windows, `--shared-module` packages a DLL instead. A shared module uses a
parameterless `dll_main` function returning `bool`:

```c
import std::win_api;

fn dll_main() -> bool {
    return std::win_api::message_box(
        "Concept DLL loaded successfully.", "Concept") != 0;
}
```

```powershell
.\build\Release\concept.exe .\examples\shared-module.concept `
    --shared-module -o example-shared.dll --vms 8
```

The compiler automatically selects `concept-runtime-shared.dll`. Its native
`DllMain` takes a temporary module reference, starts a worker thread for Concept
`dll_main`, closes the thread handle, and returns immediately. It never waits
for the VM while holding the loader lock. The worker releases its module
reference atomically when it exits, so the DLL cannot unload underneath it.
Because initialization is asynchronous, returning `false` reports worker
failure through its thread result and debugger output but cannot make the
original `LoadLibrary` call fail. Thread and detach notifications do not start
Concept code.

Source modules live in a `concept` directory and use the module filename
without its extension:

```c
import std::socket;
```

This resolves `concept/std/socket.concept`. `std` modules and classes use
qualified names such as `std::socket` and `std::Socket`. Imports are
deduplicated, may import other
modules, and report cycles as compile errors. The compiler also installs its
standard `concept` directory beside `concept.exe` so standard modules remain
available when compiling a source from another directory.

Generated programs use four cooperative VM contexts by default. Select between
1 and 64 contexts with `--vms`:

```powershell
.\build\Release\concept.exe .\examples\calculator.concept `
    -o calculator.exe --vms 8
```

## Current syntax

```c
class Counter {
    i32 value;

    fn increment() -> int {
        this.value = this.value + 1;
        return this.value;
    }
}

@complexity(25)
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
- typed functions with multiple arguments, including forward calls and
  recursion;
- typed local variables, assignment, blocks, `if`/`else`, and `while`;
- `+`, `-`, `*`, `/`, `%`, unary `-` and `!`;
- `==`, `!=`, `<`, `<=`, `>`, and `>=`;
- `input()`/`input_text()` for whole lines, plus `input_i64()` and
  `input_f64()` for parsed numeric lines;
- `print(value)` and `println(value)` for every core value type;
- the Concept-defined Windows wrapper
  `std::win_api::message_box(text, title)` and module lookup through
  `std::win_api::get_module_hadle(name)`;
- `//` line comments.
- qualified top-level imports such as `import std::socket;`.
- classes with core-type fields, optional constructors, automatic
  zero-argument construction, `object.field` access, reference-style
  assignment, methods with multiple arguments, and `this`;
- typed pointers with `T*`, address-of `&`, dereference `*`, indirect
  assignment, pointer-to-pointer values, and pointers to locals or fields;

Functions can use `@complexity(level)`, where `level` is from `0` to `100`.
Level `0` emits straight bytecode. Higher levels add progressively more opaque
branches, shuffled control-flow paths, and unreachable stack-balanced junk
instructions to that function. `@complexty(level)` is also accepted as an
alias for compatibility with the original spelling. The calculator example
uses level `100`.

Class instances are created with calls such as `Counter(42)`. A class may
declare one `constructor(...)`; inside it and every method, `this` is the
receiver object. Uninitialized declarations such as `Counter value;` construct
automatically only when no constructor arguments are required. Assignment
shares the same object, so mutations through an alias are visible through every
reference. Functions, methods, and constructors accept comma-separated typed
parameters. Multiple constructors, overloading, inheritance, and visibility
modifiers are not implemented yet.

Pointers use C-style syntax:

```c
i32 value = 41;
i32* pointer = &value;
*pointer = *pointer + 1;

// The address must be readable for a load and writable for a store.
u32* native = ptr_cast<u32>(0x10000);
u32 native_value = *native;
```

Pointers produced by `&` are checked VM references rather than native process
addresses. A null pointer is created by leaving a pointer declaration
uninitialized. Dereferencing null, invalid, or expired-local pointers stops the
VM with an error. Core locals, pointer locals, class variables, and core class
fields can be addressed; multi-level pointers and pointer fields are supported.

`ptr_cast<T>(address)` explicitly creates a `T*` into the generated program's
native address space. Its address expression must be integral, and `T` may be a
numeric or `bool` core type, or `void`. A `void*` is opaque: it can be stored,
returned, and passed as an argument, but not dereferenced, indexed, or used in
pointer arithmetic. On Windows, typed native reads and writes are checked and
produce a VM error when the requested memory is inaccessible. The programmer is
still responsible for using a correct live address and matching value type.
Native `string` and class pointers are not supported. Pointer parameters and
pointer function returns are supported; general pointer casts are not.

Strings currently support storage, function returns, printing, and `==`/`!=`.
Concatenation, indexing, and conversion between strings and numeric values are
not implemented yet. Serialized string bytes are encrypted with a fresh
per-compilation ChaCha20 key and decrypted when the runtime loads the program.
The key must be recoverable from the executable, so this hides plaintext but is
an obfuscation boundary rather than secret-key protection.

Executable programs define `main` with an integral or `bool` return type. Its
result becomes the process exit code. Shared modules instead define
`dll_main() -> bool`. Numeric values are converted as needed by initialization,
assignment, return statements, mixed arithmetic, and conditions. Narrow
integral arithmetic wraps at the operation's selected width. Integral division
and modulo by zero are runtime errors; floating-point operations use IEEE
behavior. Floating-to-integral casts reject non-finite and out-of-range values
at runtime.

## Architecture

The compiler performs this pipeline:

```text
source -> lexer/parser -> bytecode compiler -> bytecode image
       -> copy native VM stub -> append image and trailer -> program.exe/.dll
```

The runtime reads the trailer from its own executable, verifies and deserializes
the bytecode, and executes it on a stack VM. The bytecode image is versioned so
the instruction set can evolve deliberately.

Every compiler invocation divides code into instruction-aligned VM regions.
Each region receives a separate opcode-layout seed that shuffles instruction
bytes to unique values across the full `0..255` range. It then transforms every
opcode and operand byte with a per-region rolling key derived from the image key,
nonce, region boundaries, and VM seed. Each ciphertext byte feeds the next key
state, and a checksum rejects damaged encoded code before decoding. The runtime
reverses the rolling transform, reconstructs each inverse opcode mapping,
validates the regions, and cooperatively switches the active VM context as
control flow crosses region boundaries. Contexts own their code regions while
sharing the operand stack, call frames, locals, and string heap. Compiling
identical source twice therefore produces different encoded bytecode. The key
material must remain recoverable from the executable, so this is an obfuscation
boundary rather than secret-key encryption.

The same per-VM seeds also select handler mutations independently for every
opcode. Four precompiled handler shapes vary operand extraction and argument
transfer order, substitute equivalent integral arithmetic and mirrored
comparisons, and execute different side-effect-free junk calculations. Thus two
compilations can follow different native handler paths even when their Concept
behavior is identical. All handler shapes remain present in the packaged native
runtime, so this raises analysis cost but is not native machine-code mutation or
a security boundary.

Complexity decorators are also an obfuscation boundary rather than a security
guarantee. They increase reverse-engineering cost and executable size, but a
determined analyst can still recover program behavior.

Native VM, bytecode-loader, payload-reader, and runtime diagnostic strings use
`xorstr_()` so their plaintext is not stored directly in generated executables.
They are decrypted when used and can still be observed in live process memory;
this is an obfuscation boundary, not secret-key encryption. Concept source
string constants continue to use the separate per-compilation ChaCha20 bytecode
path described above.

## Standard TCP sockets

`import std::socket;` enables the `std::Socket` IPv4 TCP class, modeled after
Berkeley sockets:

```c
std::Socket server = std::Socket();
bool bound = server.bind("127.0.0.1", u16(8080));
bool listening = server.listen(16);

std::Socket client = std::Socket();
bool connected = client.connect("127.0.0.1", u16(8080));
std::Socket peer = server.accept();
i64 bytes = client.send("hello");
string chunk = peer.recv();
bool closed = client.close();
```

`valid()` reports whether construction or `accept()` produced a valid socket.
`send()` returns `-1` when it cannot send any bytes. `recv()` reads up to 4096
bytes and returns an empty string on orderly shutdown or error. `bind()` accepts
`"*"` or an empty string for all IPv4 interfaces. Sockets must be closed
explicitly.
MSVC builds implement this API with Winsock while keeping the static CRT
configuration. The runtime has no static `WS2_32.dll` import: it decodes the
module and operation names, loads the socket component, and resolves its entry
points only when a socket opcode executes. Programs that do not execute socket
operations therefore never load Winsock. This removes the obvious socket API
from the PE import table, but runtime inspection can still observe the module
after it is loaded.

Self-hosting is a later bootstrap stage: extend this subset until a Concept
compiler can be written in Concept, compile it with the C++ compiler, then package
that compiler bytecode with the same native VM. The native VM remains the small
trusted runtime included in generated executables.
