# Concept

Concept is an experimental C-like language designed to help protect compiled applications from reverse engineering. It compiles programs to bytecode and packages each one as a native executable or Windows DLL containing both the Concept virtual machine and the program bytecode.

See the [Concept language syntax reference](docs/SYNTAX.md) for the grammar,
types, operators, pointers, classes, imports, and built-in APIs implemented now.

The implementation uses C++23 and currently targets a small bootstrap language.

## Visual Studio Code

The repository includes a VS Code language extension with `.concept` file
recognition, syntax highlighting, comment and bracket behavior, indentation,
folding, and snippets.

Install the packaged extension:

```powershell
code --install-extension .\dist\concept-language-support-0.1.1.vsix --force
```

Extension sources and development instructions are in
[`editors/vscode`](editors/vscode/README.md).

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

`Windows CI` validates and packages the VS Code extension, builds the x64 MSVC
Release configuration, and runs the complete CTest suite for every push, pull
request, and manual run.

`Windows release` builds and tests a release commit, stages a self-contained
Windows x64 distribution, builds the VS Code extension, and attaches both the
versioned compiler ZIP and installable `.vsix` to the matching GitHub release.
The compiler archive contains `concept.exe`, both runtime binaries, the
`concept/std` library sources, the syntax reference, README, and third-party
notices. The VSIX version and filename come from
`editors/vscode/package.json`.

[GitHub does not trigger release workflows for draft-release creation](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release).
To attach the archive while the release is still a draft:

1. Save the draft release in GitHub.
2. Open **Actions**, select **Windows release**, and choose **Run workflow**.
3. Enter the draft's exact release tag in `release_tag`.

Publishing a release also runs this workflow automatically and replaces
existing compiler and extension assets with freshly tested builds. No
additional repository secret is required; the workflow grants the built-in
`GITHUB_TOKEN` only the repository contents permission needed to upload the
assets.

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
- `+`, `-`, `*`, `/`, `%`, shifts `<<`/`>>`, bitwise `&`/`|`/`^`, and
  unary `-`/`!`/`~`;
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
Level `0` emits straight bytecode and selects the VM's optimized handlers,
skipping per-instruction mutation calculations. Higher levels enable randomized
handler shapes and add progressively more opaque branches, shuffled control-flow
paths, and unreachable stack-balanced junk instructions to that function.
`@complexty(level)` is also accepted as an alias for compatibility with the
original spelling. The calculator example uses level `100`.

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

## Virtual machine features

Concept uses a versioned stack VM. The native VM is copied into every generated
program, while that program's bytecode image is appended to the copy:

```text
source -> lexer/parser -> bytecode compiler -> bytecode image
       -> copy native VM stub -> append image and trailer -> program.exe/.dll
```

At startup the VM finds the trailer in its own file, reads and deserializes the
embedded image, validates it, and begins at the recorded entry point. The
bytecode version is checked explicitly; an incompatible image is rejected
instead of being interpreted with the wrong instruction format.

### Execution model

- The operand stack stores 64-bit slots. Narrow integers, `bool`, `f32`, and
  `f64` retain their declared semantics through typed instructions and value
  normalization. Strings, class objects, and pointers are represented by VM
  handles rather than copied C++ objects.
- Function, method, and constructor calls use frames containing the return
  address, stack base, and local slots. Arguments are transferred into the new
  frame in source order, recursion is supported, and the call depth is limited
  to 4096 frames.
- The instruction set covers constants, locals, typed casts, integer and
  floating-point arithmetic, comparisons, conditional and unconditional
  branches, functions, class construction and fields, arrays, pointers, heap
  memory, text input/output, native calls, TCP sockets, and typed returns.
- Branch, call, and entry targets are absolute bytecode offsets. The validator
  requires every target and VM-region boundary to start on an instruction.
  Unknown opcodes, truncated operands, invalid value types, invalid string
  indexes, malformed regions, and trailing serialized data are rejected before
  execution.
- Runtime checks report stack underflow, call-stack overflow, division by zero,
  invalid numeric conversions, null or stale handles, pointer bounds errors, and
  invalid or repeated `free()` operations instead of silently continuing with a
  corrupted VM state.

### Multiple VM contexts

`--vms <count>` requests between 1 and 64 VM contexts. The compiler divides the
instruction stream at instruction boundaries, with one code region and one
mutation seed per context. If a small program has fewer instructions than the
requested count, only the number of non-empty regions that can be formed is
used.

The contexts execute cooperatively in one native thread. Before each
instruction, the dispatcher selects the context that owns the physical
instruction pointer. Branch and call operands use stable logical offsets, which
the dispatcher maps to the owning context's forward or backward physical
address. A transfer into another region therefore continues under that region's
opcode layout, direction, and handler mutations. These are VM contexts, not
parallel OS threads. They deliberately share the operand stack, call frames and
locals, string and object heaps, pointer table, and allocated heap blocks, so
state written in one context is immediately visible after a context switch.

### Per-compilation bytecode mutation

| Feature | VM behavior | Boundary |
| --- | --- | --- |
| Random opcode numbers | Every region seed builds a different one-to-one mapping from canonical instructions to byte values selected from the full `0..255` range. The runtime reconstructs the inverse mapping before dispatch. | The mapping and seed are recoverable from the image and runtime. |
| Random region direction | A seed bit selects whether the complete physical opcode-and-operand byte sequence for that region is forward or reversed. Reversed regions stay reversed after loading: the VM begins at the mirrored opcode, reads operands toward lower addresses, and dispatches the next instruction backward. | Logical branch addresses are mapped to physical addresses at runtime, so direction changes execution layout without changing Concept semantics. |
| Rolling bytecode encoding | After opcode mapping and direction selection, every opcode and operand byte is transformed with a per-region rolling state derived from the image key, nonce, region boundaries, and VM seed. Each ciphertext byte feeds the next state. | Required decoding material ships with the executable. |
| Encoded-code checksum | The loader verifies the stored encoded stream before attempting to decode it. | This detects accidental damage or simple tampering; it is not authentication against an attacker who can rewrite the executable. |
| Handler mutation | For functions above complexity `0`, every VM seed and opcode select one of four precompiled native handler shapes. The shapes reorder safe operand extraction or argument transfer, use equivalent integral arithmetic and mirrored comparisons, and perform different side-effect-free junk calculations. Complexity `0` bypasses this work. | All shapes remain in the native VM; this is runtime path variation, not generated native machine-code mutation. |
| `@complexity(n)` | The compiler can add opaque predicates, junk instructions, decoy paths, trampoline jumps, and fragmented control flow, scaled from optimized straight bytecode at `0` to the strongest available transformation at `100`. | Higher settings deliberately trade execution speed for analysis cost without changing program results or creating a security boundary. |

Because region seeds, image keys, and nonces are freshly generated, compiling
identical source twice normally produces a different bytecode image and can
select different native handler paths. Decoding must still be possible inside
the process, so these features are obfuscation layers, not secret-key encryption
or a guarantee that program behavior cannot be recovered.

### Runtime data and memory

- Source string constants are stored in the image with per-compilation ChaCha20
  encryption and are decrypted while the image is loaded. Runtime-created text
  is held in a shared VM string heap.
- Class instances live in a shared object heap. Field instructions address
  their slots, and constructors and methods use the same call-frame mechanism as
  ordinary functions.
- Fixed arrays use zero-initialized, frame-owned VM blocks. Dynamic arrays and
  `malloc<T>()` use checked shared heap blocks. Array indexing and pointer
  arithmetic preserve the element type and validate bounds.
- `&`, `*`, array decay, field pointers, pointer-to-pointer values, and
  `void*` conversions use tracked pointer records. Pointers to expired local
  variables or arrays are rejected. `free()` accepts only the base pointer of a
  live `malloc()` allocation.
- `ptr_cast<T>(address)` creates a native process-memory pointer. Native reads
  and writes are guarded on Windows and fail with a runtime error when the range
  is inaccessible. Native `string*` values and dereferencing `void*` are not
  allowed.

### I/O, native integration, and packaging

- `input_text()`, integral input, and floating-point input read from standard
  input. `print()` and `println()` format supported core values to standard
  output.
- Socket instructions implement the `std::Socket` API described below. On
  Windows, Winsock is loaded and its functions are resolved only when a socket
  instruction actually executes, so programs that do not use sockets do not
  load `WS2_32.dll`.
- The generic Windows x64 native-call bridge resolves a requested module and
  symbol at runtime and supports up to four integral, Boolean, string, or
  non-string pointer arguments. `std::win_api` supplies Concept-language
  wrappers over this bridge; the VM does not need a separate opcode for every
  WinAPI function.
- An executable runs Concept `main` and converts its declared return value to a
  process exit code. A Windows `--shared-module` build exports a native
  `DllMain`, starts a worker thread on process attach, and runs Concept
  `dll_main` outside the loader lock.
- MSVC runtime stubs use the static CRT. Native VM, bytecode-loader,
  payload-reader, and diagnostic strings use `xorstr_()` so their plaintext is
  not stored directly in the generated file. The text is decoded when used and
  can still be observed in live process memory.

The compiler and core VM remain portable C++23 code. Platform-specific modules
are gated: `std::win_api` and shared-module output are rejected on non-Windows
hosts, the current generic native-call bridge requires Windows x64, and guarded
native pointer dereferencing is currently implemented for Windows. A remaining
unsupported runtime operation fails explicitly instead of silently using
different behavior.

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
u8* binary = malloc(1024);
i64 binary_count = peer.recv_bytes(binary, 1024);
bool closed = client.close();
```

`valid()` reports whether construction or `accept()` produced a valid socket.
`send()` and `send_bytes(u8*, count)` send the complete supplied range or
return `-1` when they cannot send any bytes. `recv()` reads up to 4096 text
bytes. `recv_bytes(u8*, count)` reads one binary block and returns its size,
`0` for shutdown, or `-1` for error. `bind()` accepts `"*"` or an empty string
for all IPv4 interfaces. Sockets must be closed explicitly.
MSVC builds implement this API with Winsock while keeping the static CRT
configuration. The runtime has no static `WS2_32.dll` import: it decodes the
module and operation names, loads the socket component, and resolves its entry
points only when a socket opcode executes. Programs that do not execute socket
operations therefore never load Winsock. This removes the obvious socket API
from the PE import table, but runtime inspection can still observe the module
after it is loaded.

## Concept HTTPS profile

`import std::https;` provides an HTTPS GET client whose TLS 1.3 record layer,
handshake, transcript, key schedule, CertificateVerify authentication, and HTTP
formatting are implemented in `.concept` sources. By default on Windows, the VM
passes the peer's DER certificate chain and requested hostname to the operating
system certificate store. Concept then extracts the authenticated leaf public
key and verifies the TLS CertificateVerify message itself. The VM does not call
a system TLS library and has no TLS or HTTP opcode.

The implemented profile is intentionally narrow:

- X25519 key agreement;
- `TLS_CHACHA20_POLY1305_SHA256` (`0x1303`);
- either `rsa_pss_rsae_sha256` (`0x0804`) with a 2048-bit RSA modulus,
  exponent 65537, and a 32-byte PSS salt, or
  `ecdsa_secp256r1_sha256` (`0x0403`) with an uncompressed P-256 public key;
- Windows system-store chain and hostname validation by default, or an exact
  SHA-256 leaf-certificate pin plus its RSA/ECDSA public key when explicit pin
  fields are supplied;
- HTTP/1.1 GET with `Connection: close`, authenticated `close_notify`, and the
  raw response returned to the caller.

The pure-Concept P-256 verifier uses a nine-limb radix-29 representation,
precomputed Montgomery constants, reusable arithmetic workspaces, and fused VM
indexed memory instructions. These optimizations do not add native crypto
operations or weaken certificate/signature validation.

[`examples/https-libomath.concept`](examples/https-libomath.concept) demonstrates
the normal no-pin API. [`examples/https.concept`](examples/https.concept) and
[`examples/https-ecdsa.concept`](examples/https-ecdsa.concept) demonstrate the
optional RSA and P-256 pin modes. Supplying any pin-related field selects pin
mode, and incomplete pin settings fail instead of silently falling back to the
system store. A failed chain, hostname, pin, CertificateVerify signature,
Finished MAC, AEAD tag, selected cipher/group/version, or malformed record makes
`get()` fail closed and places a short diagnostic in `error`.

The Windows certificate component (`Crypt32`) and its entry points are resolved
only when system validation executes; they are not static PE imports and their
names are protected with `xorstr_`. System validation checks the chain against
Windows trusted roots and applies the SSL hostname policy. It does not force an
online revocation check. On non-Windows systems the default system-trust mode is
currently unavailable, while explicit pin mode remains usable.

This is an educational implementation, not a production TLS stack. It does not
implement AES-GCM, session resumption, HelloRetryRequest, client certificates,
IDNA conversion, or constant-time audited cryptography. Explicit pins must be
updated deliberately when the server certificate or key changes.

Self-hosting is a later bootstrap stage: extend this subset until a Concept
compiler can be written in Concept, compile it with the C++ compiler, then package
that compiler bytecode with the same native VM. The native VM remains the small
trusted runtime included in generated executables.
