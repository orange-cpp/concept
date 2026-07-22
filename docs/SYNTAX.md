# Concept language syntax

This document describes the syntax and behavior implemented by the current
Concept compiler. It is a reference for the bootstrap language, not a roadmap
for features that may be added later.

## Source files

Concept source files use the `.concept` extension. Source is case-sensitive.
Whitespace separates tokens but is otherwise insignificant.

An identifier starts with a letter or `_` and continues with letters, digits,
or `_`. A qualified identifier joins segments with `::`, for example
`std::Socket` or `std::socket`.

Only line comments are supported:

```c
// Everything after // is ignored until the end of the line.
```

## Core types

| Type | Aliases | Meaning | Default |
|---|---|---|---|
| `bool` | — | Boolean | `false` |
| `i8` | — | Signed 8-bit integer | `0` |
| `i16` | — | Signed 16-bit integer | `0` |
| `i32` | `int` | Signed 32-bit integer | `0` |
| `i64` | — | Signed 64-bit integer | `0` |
| `u8` | — | Unsigned 8-bit integer | `0` |
| `u16` | — | Unsigned 16-bit integer | `0` |
| `u32` | — | Unsigned 32-bit integer | `0` |
| `u64` | — | Unsigned 64-bit integer | `0` |
| `f32` | `float` | 32-bit floating point | `0.0` |
| `f64` | `double` | 64-bit floating point | `0.0` |
| `string` | `text` | Text value | `""` |

Fixed-width integral results are normalized to their declared width. Numeric
values can be converted implicitly when assigned, returned, passed to a native
method, or used with a different numeric operand type. Strings do not convert
to or from numeric values.

## Literals

```c
i64 decimal = 42;
u64 hexadecimal = 0x10000;
f64 fraction = 1.5;
f64 scientific = 2.5e2;
bool enabled = true;
bool disabled = false;
string message = "line one\nline two";
```

Decimal and hexadecimal integer literals are accepted up to the `u64` range.
An integer literal is inferred as `i64` when it fits, otherwise as `u64`.
Floating-point literals are inferred as `f64`. A leading `-` is a unary
operator rather than part of a literal.

String literals support these escapes:

| Escape | Value |
|---|---|
| `\\` | Backslash |
| `\"` | Double quote |
| `\n` | Newline |
| `\r` | Carriage return |
| `\t` | Tab |
| `\0` | Null byte |

## Program structure

A source file contains imports, class declarations, and function declarations:

```ebnf
program           = { import | class-declaration | function-declaration } ;
import            = "import", qualified-identifier, ";" ;
qualified-identifier = identifier, { "::", identifier } ;
core-type         = "bool" | "i8" | "i16" | "i32" | "i64"
                  | "u8" | "u16" | "u32" | "u64"
                  | "f32" | "f64" | "int" | "float" | "double"
                  | "string" | "text" ;
generic-parameter = identifier ;
generic-class-type = qualified-identifier, "<", core-type, ">" ;
local-type        = ( core-type | "void" | qualified-identifier
                    | generic-class-type ), { "*" } ;
class-declaration = "class", qualified-identifier,
                    [ "<", identifier, ">" ], "{",
                    { field-declaration | constructor-declaration
                    | function-declaration }, "}" ;
field-declaration = ( core-type | "void" | generic-parameter ),
                    { "*" }, identifier, ";" ;
constructor-declaration = "constructor", "(", [ parameters ], ")", block ;
function-declaration = { decorator }, "fn", identifier,
                       "(", [ parameters ], ")", "->",
                       ( core-type | "void" | generic-parameter ),
                       { "*" }, block ;
parameters        = parameter, { ",", parameter } ;
parameter         = local-type, identifier ;
decorator         = "@complexity(", decimal-integer, ")" ;
block             = "{", { statement }, "}" ;
```

The required entry point depends on the selected output mode.

| Compiler mode | Required entry | Result |
|---|---|---|
| Default executable | `fn main() -> bool` or an integral type | Process exit code |
| Windows `--shared-module` | `fn dll_main() -> bool` | Worker success status |

Both entry functions must have no parameters. A shared module is packaged with
`concept-runtime-shared.dll`. During `DLL_PROCESS_ATTACH`, native `DllMain`
takes a temporary reference to the module, creates a worker thread, closes its
thread handle, and immediately returns. It does not execute or wait for the VM
under the loader lock. The worker invokes Concept `dll_main` and atomically
releases the module reference as it exits, preventing the DLL from unloading
while Concept code is active.

Initialization is therefore asynchronous. Returning `false` marks the worker
as failed and writes a debugger diagnostic, but it cannot make the already
completed `LoadLibrary` call fail. Concept code is not started for thread
attach, thread detach, or process detach. Operations such as `message_box` and
sockets run from the worker rather than the native `DllMain` callback.

Functions accept zero or more comma-separated typed parameters and return a
core value or pointer:

```c
fn add(i32 left, i32 right) -> i32 {
    return left + right;
}

fn main() -> int {
    return add(19, 23);
}
```

Calls may refer to functions declared later in the merged program. Reaching the
end of a function without `return` returns that function's default value.
Parameters may be core values, pointers, class objects, or concrete generic
class specializations. Arguments use the same numeric conversions as
assignment; pointer and class arguments must have matching types. Calls require
the exact declared argument count. Pointer returns use the declared pointee type
and indirection depth. Class and no-value returns are not currently available;
`void` is legal only as the base of a pointer type such as `void*`.

## Variables and assignment

A local declaration contains a type, a name, an optional initializer, and a
semicolon:

```c
i32 count;
i32 answer = 42;
string label = "ready";
```

Core locals receive their default value when no initializer is present. A class
local with no initializer automatically creates an instance. An uninitialized
pointer is null.

Assignment targets may be locals, fields, indexed elements, or dereferenced
pointers:

```c
answer = 40 + 2;
object.value = answer;
values[2] = answer;
*pointer = answer;
```

Blocks do not create separate local-variable scopes in the current compiler.
Local names are function-scoped and cannot be redeclared elsewhere in the same
function.

## Arrays and heap memory

A fixed-size local array places a positive constant length after its name:

```c
i32 values[4];
string labels[2];
i32* pointers[8];

values[0] = 40;
values[1] = 2;
i32 answer = values[0] + values[1];
```

Array elements are zero-initialized. An array name converts to a pointer to its
first element, so indexing also works on pointers. Array initializers and arrays
of class objects are not currently supported. Every indexed read or write is
bounds-checked by the VM. A pointer to a local array expires when its declaring
function returns.

`malloc(byte_count)` allocates zero-initialized bytes from the shared VM heap.
Its result adopts the pointer type of its destination:

```c
i32* memory = malloc(4 * 10);
memory[0] = 42;
i32 value = *(memory + 0);
free(memory);
```

`free(pointer)` releases a block created by `malloc` and returns `0`. The pointer
must be the original base pointer. Passing null is allowed. Freeing an array,
freeing the same block twice, dereferencing freed memory, or accessing outside a
block stops execution with a VM error. Heap and array state is shared by every
VM context in a multi-VM executable.

## Statements and control flow

```ebnf
statement = block
          | variable-declaration, ";"
          | assignment, ";"
          | expression, ";"
          | "return", expression, ";"
          | "if", "(", expression, ")", statement,
            [ "else", statement ]
          | "while", "(", expression, ")", statement ;
variable-declaration = local-type, identifier,
                       [ "[", integer-literal, "]" | "=", expression ] ;
```

Examples:

```c
if (answer == 42) {
    println("correct");
} else {
    println("incorrect");
}

while (answer < 42) {
    answer = answer + 1;
}
```

Numeric and `bool` expressions can be used as conditions. Zero is false and a
nonzero value is true. Strings, pointers, and objects cannot be conditions.
There are no `for`, `do`, `switch`, `break`, or `continue` statements yet.

## Expressions and precedence

Operators are listed from highest to lowest precedence:

| Level | Syntax | Notes |
|---|---|---|
| Primary | literals, names, `call()`, casts, `(expression)` | — |
| Postfix | `value[index]`, `object.field`, `object.method()` | Left-associative |
| Unary | `-value`, `!value`, `~value`, `&value`, `*pointer` | Right-associative |
| Multiplicative | `*`, `/`, `%` | `%` requires integral operands |
| Additive | `+`, `-` | Numeric operands or supported pointer arithmetic |
| Shift | `<<`, `>>` | Integral operands; result uses the left type |
| Relational | `<`, `<=`, `>`, `>=` | Numeric operands only |
| Equality | `==`, `!=` | Numeric, `bool`, or matching strings |
| Bitwise AND | `&` | Integral operands only |
| Bitwise XOR | `^` | Integral operands only |
| Bitwise OR | `|` | Integral operands only |

Parentheses override precedence. In particular, dereference a class pointer
before selecting a field:

```c
(*box_pointer).value
```

There are currently no logical `&&` or `||`, increment or decrement operators,
compound assignments, or ternary expressions. Bitwise and shift operators
require integral operands. A shift count greater than or equal to the left
operand width is a runtime error; signed right shift is arithmetic. Pointer
arithmetic supports `pointer + integer`, `integer + pointer`, and `pointer -
integer`; class objects cannot be used with binary operators.

## Core value casts

A core type name followed by parentheses performs a value conversion:

```c
i32 whole = i32(42.75);
f64 precise = f64(whole);
u16 port = u16(8080);
bool present = bool(whole);
```

Numeric and `bool` core types can be converted. Floating-to-integral conversion
truncates toward zero and reports a runtime error when the value is non-finite
or outside the target range. `string`, pointer, and class conversions are not
supported by this syntax.

## Pointers

Append one or more `*` tokens to a type to declare pointers:

```c
i32 value = 40;
i32* pointer = &value;
i32** pointer_to_pointer = &pointer;

*pointer = *pointer + 1;
**pointer_to_pointer = **pointer_to_pointer + 1;
```

`&` accepts a local, a class field, or a dereference expression. `*` reads the
pointee, and a dereference on the left side of `=` writes it. Pointer assignment
requires an exact pointee type and indirection depth.

Pointers created with `&` are checked VM references, not exposed native
addresses. They may refer to core locals, pointer locals, class locals, or core
class fields. A pointer to a local becomes invalid when that function returns.
Dereferencing a null, invalid, or expired-local pointer stops execution with a
VM error. Object-field references remain valid because objects live for the
duration of program execution.

Class pointers are supported for locals:

```c
Box box;
Box* pointer = &box;
(*pointer).value = 42;
```

Class fields themselves currently use core types, but a core pointer such as
`i32*` is a valid field type.

### Native address casts

`ptr_cast<T>(address)` converts an integral address into `T*`:

```c
u32* mapped = ptr_cast<u32>(0x10000);
u32 current = *mapped;
*mapped = current + 1;
```

`T` must be a numeric or `bool` core type, or `void`. A concrete core type
determines the number and interpretation of bytes read or written. A `void*` is
an opaque native address: it can be stored, returned, or passed to another
function, but it cannot be dereferenced, indexed, or used in pointer arithmetic.
The address belongs to the generated program's own native process and must be
valid for the requested operation. Address `0` produces a null pointer.

Guarded native dereferencing is currently implemented on Windows. An
inaccessible address produces a VM error rather than a normal Concept value.
Other hosts report native dereferencing as unavailable. Native `string` and
class pointers are not supported.

`ptr_cast` is intentionally explicit because native memory access bypasses the
safety of normal VM references. Element-scaled pointer arithmetic and indexing
work on typed native pointers. Pointer comparisons and general
pointer-to-pointer casts are not implemented yet.

## Classes

Classes contain fields, methods, and optionally one constructor:

```c
class Counter {
    i32 value;

    constructor(i32 initial) {
        this.value = initial;
    }

    fn increment(i32 amount) -> int {
        this.value = this.value + amount;
        return this.value;
    }
}

fn main() -> int {
    Counter counter = Counter(40);
    return counter.increment(2);
}
```

Inside a method or constructor, `this` is the receiver object. A class can be
constructed with `ClassName(arguments)`. A class local without an initializer
is constructed automatically when the class has no constructor or declares a
zero-argument constructor. A constructor that requires arguments must be called
explicitly. Constructors have no return type and cannot return a value.

Class assignment shares the same object rather than copying its fields:

```c
Counter first;
Counter alias = first;
alias.value = 42;
// first.value is now 42 too.
```

Methods accept the same parameter types as top-level functions and return core
values. Class-valued fields, inheritance, visibility modifiers, static members,
multiple constructors, and method overloading are not implemented.

## Generic classes

A class may declare one generic type parameter and use it for fields, pointer
fields, locals, fixed arrays, parameters, constructor parameters, and method
return values:

```c
class Box<T> {
    T value;

    constructor(T initial) {
        this.value = initial;
    }

    fn get() -> T {
        return this.value;
    }
}

fn main() -> int {
    Box<float> box = Box<float>(42.0);
    return i32(box.get());
}
```

Each used specialization is monomorphized into independent bytecode. Generic
arguments must currently be core types, and only one type parameter is
supported. Generic functions, nested generic arguments, pointer arguments such
as `Box<i32*>`, and class arguments such as `Box<Counter>` are not available.

## Imports and qualified names

Imports resolve a module name to a `.concept` file beneath a `concept`
directory:

```c
import math;        // concept/math.concept
import std::socket; // concept/std/socket.concept
```

`::` components become path components during module lookup. Imported classes
and functions are merged into the program. Duplicate imports are loaded once,
and import cycles are compile errors. Importing a file does not automatically
add a namespace to its declarations; standard-library declarations explicitly
use `std::` names.

The command-line compiler also searches the `concept` directory installed next
to `concept.exe`, allowing bundled standard modules to be imported from source
files in other directories.

## Complexity decorator

`@complexity(level)` is a compiler instruction placed immediately before a
function or method:

```c
@complexity(75)
fn protected_calculation() -> i32 {
    return 6 * 7;
}
```

`level` is a decimal integer from `0` through `100`. Level `0` emits straight
bytecode. Higher values add progressively more opaque predicates, junk work,
and control-flow jumps. The legacy spelling `@complexty(level)` is accepted.
Only one complexity decorator may be attached to a function.

This decorator raises reverse-engineering cost but is not a security boundary.
Every compilation also independently randomizes opcode numbers for each VM
context. Each VM seed selects one of four equivalent native handler shapes per
opcode. These shapes may reorder safe operand work, substitute modular integer
operations or mirrored comparisons, and perform side-effect-free junk
calculations. Handler selection changes across compilations automatically and
does not require a decorator. The runtime contains every precompiled shape, so
this is execution-path obfuscation rather than native machine-code mutation.

Serialized bytecode also encodes every opcode and operand with per-VM rolling
keys. Each VM seed independently chooses whether its physical region is stored
forward or byte-reversed before rolling encoding. A ciphertext byte changes the
key state used for the next byte, and the runtime verifies a checksum before
reversing the transform, direction, and randomized opcode mapping. This encoding
is automatic at every complexity level. Its key material is stored in
recoverable form in the packaged program, so it hides static plaintext patterns
but is not secret-key protection.

## Built-in input and output

| Function | Return | Behavior |
|---|---|---|
| `input()` | `string` | Reads one line of text |
| `input_text()` | `string` | Alias of `input()` |
| `input_i64()` | `i64` | Reads and parses one signed integer line |
| `input_f64()` | `f64` | Reads and parses one floating-point line |
| `print(value)` | `i64` | Prints one core value without a newline |
| `println(value)` | `i64` | Prints one core value followed by a newline |

Input parse failures stop the VM with an error. `print` and `println` accept
core values, but not pointers or class objects. Both return `0`, so they can be
used as expression statements.

## `std::win_api`

`std::win_api` contains Windows API wrappers implemented in Concept source.
It is available only when the compiler itself targets Windows x64; importing it
on Linux, macOS, or 32-bit Windows is a compile-time error.

```c
import std::win_api;

i32 result = std::win_api::message_box(
    "Concept DLL loaded successfully.", "Concept");

void* kernel32 = std::win_api::get_module_hadle("kernel32.dll");
```

| Function | Return | Behavior |
|---|---|---|
| `std::win_api::message_box(string text, string title)` | `i32` | Show a Windows information dialog and return its native result |
| `std::win_api::get_module_hadle(string module_name)` | `void*` | Return the loaded module handle from `GetModuleHandleA`, or null if it is not loaded |

The wrapper in `concept/std/win_api.concept` supplies the module name, symbol
name, null owner, and information-dialog flags. The compiler and VM do not have
a MessageBox-specific instruction. All Windows wrappers share one internal
native-call instruction that resolves the requested module and symbol lazily.

The initial Windows x64 native-call ABI accepts zero to four integral, `bool`,
string, or non-string pointer arguments and returns one native word, which a
wrapper casts to its public type. Floating arguments, structures passed by
value, callbacks, and calls with more than four arguments are not supported
yet. String pointers are borrowed only for the duration of the call. Module and
symbol strings remain encrypted in packaged bytecode, and no static import is
added for the requested Windows component.

An incorrect module name, symbol name, argument count, or native signature is a
programming error in the wrapper. The VM checks the supported argument kinds,
but it cannot recover safely from calling a native symbol with the wrong ABI.

## `std::array<T>`

`std::array<T>` is a growable container implemented entirely in Concept source
for any core value type. Import it with `import std::array;`:

```c
import std::array;

fn main() -> int {
    std::array<float> values;
    values.value = 42.5;
    values.push();

    values.index = 0;
    float answer = values.get();
    values.destroy();
    return i32(answer);
}
```

This standard-library class retains its field-based interface: `value` supplies
a `T` argument and `index` supplies a `u64` argument before an operation. The
container also exposes its current `length` and `capacity` fields.

| Method | Return | Meaning |
|---|---|---|
| `push()` | `bool` | Append `value`, growing capacity geometrically |
| `valid()` | `bool` | Whether `index` is less than the current length |
| `get()` | `T` | Read `index`, or return the default `T` value when invalid |
| `set()` | `bool` | Store `value` at a valid `index` |
| `pop()` | `bool` | Remove the last value and place it in `value` |
| `clear()` | `bool` | Remove all logical elements without releasing capacity |
| `size()` | `u64` | Return the current element count |
| `empty()` | `bool` | Whether the array has no elements |
| `destroy()` | `bool` | Release storage; safe to call again |

There is no automatic destructor yet, so call `destroy()` when the container is
no longer needed. Its growth, copying, indexing, and cleanup are defined in
`concept/std/array.concept`; the VM has no special dynamic-array implementation.

## `std::http`

`std::http` is an HTTP/1.0 GET client implemented in Concept on top of
`std::Socket`. Importing it automatically imports the socket module:

```c
import std::http;

fn main() -> int {
    std::http request;
    request.host = "example.com";
    request.path = "/";
    request.port = u16(80);

    if (!request.get()) {
        return 1;
    }
    print(request.response);
    return 0;
}
```

Set `host`, `path`, and `port`, then call `get()`. An empty path defaults to
`"/"`, and port `0` defaults to port `80`. `get()` returns whether a non-empty
response was received and places the raw HTTP response in `response`. Hostnames
are resolved by the underlying socket connection.

The current socket text receive operation returns at most 4096 bytes, so
`response` contains the first response block. The module does not yet decode
headers, redirects, chunked transfer encoding, or content encodings. Use
`std::https` for the TLS 1.3 profile below. All HTTP request formatting
and control flow are defined in Concept source; the VM contains no
HTTP-specific implementation.

## Binary buffers and cryptography

The VM exposes five binary and host-trust helpers used by the Concept standard
library:

| Function | Meaning |
|---|---|
| `entropy_fill(u8* output, count)` | Fill a checked VM byte range from host entropy |
| `string_length(string value)` | Return the byte length of text |
| `string_byte(string value, index)` | Read one checked text byte |
| `string_from_bytes(u8* input, count)` | Create text preserving every byte, including zero |
| `system_verify_x509(string host, u8* chain, count)` | Validate a length-prefixed DER chain and hostname with the host certificate policy; currently implemented on Windows |

`std::bytes` in `concept/std/bytes.concept` owns a growable `u8` buffer. Its
methods include `reserve`, `resize`, `append`, big-endian `append_u16`/`u24`/
`u32`, `append_bytes`, `append_text`, `read_u16`/`u24`/`u32`, `fill_random`,
`to_string`, and `destroy`. There is no automatic destructor, so callers that
retain a buffer should call `destroy()`.

`std::crypto` implements SHA-256, HMAC-SHA256, HKDF-SHA256 and TLS 1.3 HKDF
labels, ChaCha20, Poly1305, ChaCha20-Poly1305, and X25519 in Concept source.
`std::rsa` implements only RSA-PSS/SHA-256 verification for a 2048-bit modulus,
exponent 65537, and 32-byte salt. `std::ecdsa` implements strict-DER
ECDSA/SHA-256 verification over NIST P-256, including public-point validation.
These modules operate on caller-sized `u8*` buffers and are educational code,
not constant-time or independently audited.

## `std::https`

`std::https` performs a TLS 1.3 HTTP/1.1 GET with its protocol and
cryptographic operations implemented in Concept source:

```c
import std::https;

fn main() -> int {
    std::https request;
    request.host = "api.example.com";
    request.path = "/status";
    request.port = u16(443);

    if (!request.get()) {
        println(request.error);
        return 1;
    }
    print(request.response);
    return 0;
}
```

With every pin length left at zero, `get()` uses the operating system trust
store. On Windows it validates the peer's full X.509 chain and requested
hostname, then extracts the authenticated leaf key in Concept and verifies the
TLS CertificateVerify signature. The host boundary loads `Crypt32` and resolves
its certificate functions only when this path executes; there is no static
`Crypt32.dll` import. Hostnames should be ASCII or already IDNA-encoded.

Explicit pinning is optional. Supplying any pin-related field selects pin mode,
which requires the exact SHA-256 of the leaf certificate DER and either its
2048-bit RSA modulus or 65-byte uncompressed P-256 SEC1 point:

```c
u8* leaf_pin = malloc(32);
u8* public_key = malloc(65); // 0x04 || 32-byte X || 32-byte Y
// Fill both buffers through a trusted channel.
request.certificate_sha256 = leaf_pin;
request.certificate_sha256_length = 32;
request.ecdsa_public_key = public_key;
request.ecdsa_public_key_length = 65;
```

For RSA pinning, set `rsa_modulus` and `rsa_modulus_length = 256` instead. If
both valid key lengths are configured, the ClientHello advertises both schemes
and verifies whichever one the server selects. An incomplete pin configuration
fails immediately and never falls back to system trust.

Port `0` defaults to `443`, and an empty path defaults to `"/"`. On success,
`response` contains the complete raw HTTP response received before TLS shutdown.
On failure, `error` describes the failed stage. The complete example at
`examples/https-libomath.concept` uses normal system trust;
`examples/https.concept` shows RSA pin decoding and
`examples/https-ecdsa.concept` shows P-256 pinning. The peer must send an
authenticated TLS `close_notify`; an unauthenticated TCP EOF is rejected to
avoid treating a truncated raw response as complete.

The current interoperable profile requires TLS 1.3, X25519, and
`TLS_CHACHA20_POLY1305_SHA256`. CertificateVerify may use either
`rsa_pss_rsae_sha256` with a 2048-bit RSA key using exponent 65537 or
`ecdsa_secp256r1_sha256` with a P-256 key. In default mode the Windows SSL chain
policy checks trusted roots, certificate validity and usage, and hostname; an
online revocation check is not forced. In pin mode the client instead validates
the exact leaf DER pin and verifies CertificateVerify with the separately
pinned public key. Both modes verify Finished and every AEAD tag and reject
malformed or unsupported handshake data. Default system trust currently returns
failure on non-Windows hosts; explicit pin mode remains cross-platform.

There is no AES-GCM, HelloRetryRequest, session resumption, KeyUpdate, client
certificate, automatic IDNA conversion, HTTP redirect, content decoding, or
chunked-body decoder. The cryptographic code has not been audited for
constant-time behavior and is not production-safe.

## `std::Socket`

Import the standard socket module before using its qualified class:

```c
import std::socket;

fn main() -> int {
    std::Socket client = std::Socket();
    if (!client.valid()) {
        return 1;
    }
    bool connected = client.connect("127.0.0.1", u16(8080));
    client.close();
    return connected;
}
```

The current IPv4 TCP API is:

| Method | Return | Meaning |
|---|---|---|
| `valid()` | `bool` | Whether the socket handle is valid |
| `connect(string host, u16 port)` | `bool` | Connect to a remote IPv4 TCP endpoint |
| `bind(string host, u16 port)` | `bool` | Bind a local endpoint; `""` or `"*"` means all interfaces |
| `listen(i32 backlog)` | `bool` | Start listening |
| `accept()` | `std::Socket` | Accept one connection |
| `send(string data)` | `i64` | Return bytes sent, or `-1` when nothing could be sent |
| `recv()` | `string` | Read up to 4096 bytes; empty means shutdown or error |
| `send_bytes(u8* data, count)` | `i64` | Send a checked binary range |
| `recv_bytes(u8* data, count)` | `i64` | Read one binary block into a checked range |
| `close()` | `bool` | Close the socket |

Sockets are not closed automatically. Socket support is loaded lazily when a
socket operation executes.

## Compact grammar

This grammar summarizes the parser. Semantic type restrictions described above
still apply.

```ebnf
return-type    = ( core-type | "void" | generic-parameter ), { "*" } ;
parameter      = local-type, identifier ;
parameters     = parameter, { ",", parameter } ;
function       = { decorator }, "fn", identifier,
                 "(", [ parameters ], ")", "->", return-type, block ;
constructor    = "constructor", "(", [ parameters ], ")", block ;

statement      = block
               | local-type, identifier,
                 [ "[", integer-literal, "]" | "=", expression ], ";"
               | lvalue, "=", expression, ";"
               | "return", expression, ";"
               | "if", "(", expression, ")", statement,
                 [ "else", statement ]
               | "while", "(", expression, ")", statement
               | expression, ";" ;

lvalue         = identifier | member-expression | index-expression
               | "*", unary-expression ;
expression     = bitwise-or ;
bitwise-or     = bitwise-xor, { "|", bitwise-xor } ;
bitwise-xor    = bitwise-and, { "^", bitwise-and } ;
bitwise-and    = equality, { "&", equality } ;
equality       = comparison, { ( "==" | "!=" ), comparison } ;
comparison     = shift, { ( "<" | "<=" | ">" | ">=" ), shift } ;
shift          = additive, { ( "<<" | ">>" ), additive } ;
additive       = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" | "%" ), unary } ;
unary          = ( "-" | "!" | "~" | "&" | "*" ), unary | postfix ;
postfix        = primary,
                 { "[", expression, "]"
                 | ".", identifier, [ "(", [ arguments ], ")" ] } ;
primary        = literal
               | identifier
               | identifier, "(", [ arguments ], ")"
               | generic-class-type, "(", [ arguments ], ")"
               | core-type, "(", expression, ")"
               | "ptr_cast", "<", ( core-type | "void" ), ">",
                 "(", expression, ")"
               | "(", expression, ")" ;
arguments      = expression, { ",", expression } ;
```

## Current omissions

The current language does not yet provide:

- default arguments, variadic parameters, or function/method overloading;
- generic functions, multiple generic parameters, or non-core generic
  arguments;
- class or no-value (`void`) function returns;
- object destruction;
- `for`, `do`, `switch`, `break`, or `continue`;
- logical short-circuit, compound-assignment, increment, decrement, or ternary
  operators;
- string concatenation, indexing, or numeric conversion;
- multiple constructors, class-valued fields, inheritance, visibility, or
  static members;
- pointer comparison or general pointer casts.

These omissions are syntax errors or compile-time type errors rather than
silently accepted features.
