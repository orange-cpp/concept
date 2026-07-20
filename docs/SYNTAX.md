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
local-type        = ( core-type | qualified-identifier ), { "*" } ;
class-declaration = "class", identifier, "{",
                    { field-declaration | function-declaration }, "}" ;
field-declaration = core-type, { "*" }, identifier, ";" ;
function-declaration = { decorator }, "fn", identifier,
                       "(", ")", "->", core-type, block ;
decorator         = "@complexity(", decimal-integer, ")" ;
block             = "{", { statement }, "}" ;
```

Every program must define a top-level `main` function. `main` must return
`bool` or an integral type; its result becomes the executable's process exit
code.

Functions currently have no parameters and return one core type:

```c
fn answer() -> i32 {
    return 42;
}

fn main() -> int {
    return answer();
}
```

Calls may refer to functions declared later in the merged program. Reaching the
end of a function without `return` returns that function's default value.
Pointer, class, and no-value return types are not currently available.

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

Assignment targets may be locals, fields, or dereferenced pointers:

```c
answer = 40 + 2;
object.value = answer;
*pointer = answer;
```

Blocks do not create separate local-variable scopes in the current compiler.
Local names are function-scoped and cannot be redeclared elsewhere in the same
function.

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
variable-declaration = local-type, identifier, [ "=", expression ] ;
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
| Postfix | `object.field`, `object.method()` | Left-associative |
| Unary | `-value`, `!value`, `&value`, `*pointer` | Right-associative |
| Multiplicative | `*`, `/`, `%` | `%` requires integral operands |
| Additive | `+`, `-` | Numeric operands only |
| Relational | `<`, `<=`, `>`, `>=` | Numeric operands only |
| Equality | `==`, `!=` | Numeric, `bool`, or matching strings |

Parentheses override precedence. In particular, dereference a class pointer
before selecting a field:

```c
(*box_pointer).value
```

There are currently no logical `&&` or `||`, bitwise operators, increment or
decrement operators, compound assignments, or ternary expressions. Pointers
and class objects cannot be used with binary operators.

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

`T` must be a numeric or `bool` core type. It determines the number and
interpretation of bytes read or written. The address belongs to the generated
program's own native process and must be valid for the requested operation.
Address `0` produces a null pointer.

Guarded native dereferencing is currently implemented on Windows. An
inaccessible address produces a VM error rather than a normal Concept value.
Other hosts report native dereferencing as unavailable. Native `string` and
class pointers are not supported.

`ptr_cast` is intentionally explicit because native memory access bypasses the
safety of normal VM references. Pointer arithmetic, pointer comparisons,
general pointer-to-pointer casts, and pointer function parameters or returns are
not implemented yet.

## Classes

Classes contain core fields and no-argument methods:

```c
class Counter {
    i32 value;

    fn increment() -> int {
        this.value = this.value + 1;
        return this.value;
    }
}

fn main() -> int {
    Counter counter = Counter();
    counter.value = 41;
    return counter.increment();
}
```

Inside a method, `this` is the receiver object. A class can be constructed with
`ClassName()` or by declaring a class local without an initializer. Constructors
do not accept arguments.

Class assignment shares the same object rather than copying its fields:

```c
Counter first;
Counter alias = first;
alias.value = 42;
// first.value is now 42 too.
```

Methods return core values and currently take no parameters. Class-valued
fields, user-defined constructors, inheritance, visibility modifiers, static
members, and method overloading are not implemented.

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
context.

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
| `close()` | `bool` | Close the socket |

Sockets are not closed automatically. Socket support is loaded lazily when a
socket operation executes.

## Compact grammar

This grammar summarizes the parser. Semantic type restrictions described above
still apply.

```ebnf
statement      = block
               | local-type, identifier, [ "=", expression ], ";"
               | lvalue, "=", expression, ";"
               | "return", expression, ";"
               | "if", "(", expression, ")", statement,
                 [ "else", statement ]
               | "while", "(", expression, ")", statement
               | expression, ";" ;

lvalue         = identifier | member-expression | "*", unary-expression ;
expression     = equality-expression ;
equality       = comparison, { ( "==" | "!=" ), comparison } ;
comparison     = additive, { ( "<" | "<=" | ">" | ">=" ), additive } ;
additive       = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" | "%" ), unary } ;
unary          = ( "-" | "!" | "&" | "*" ), unary | postfix ;
postfix        = primary,
                 { ".", identifier, [ "(", [ arguments ], ")" ] } ;
primary        = literal
               | identifier
               | identifier, "(", [ arguments ], ")"
               | core-type, "(", expression, ")"
               | "ptr_cast", "<", core-type, ">", "(", expression, ")"
               | "(", expression, ")" ;
arguments      = expression, { ",", expression } ;
```

## Current omissions

The current language does not yet provide:

- function or user-method parameters;
- pointer, class, or `void` function returns;
- arrays, indexing, allocation, or object destruction;
- `for`, `do`, `switch`, `break`, or `continue`;
- logical short-circuit, bitwise, compound-assignment, increment, decrement, or
  ternary operators;
- string concatenation, indexing, or numeric conversion;
- constructors with arguments, class-valued fields, inheritance, visibility,
  static members, or overloading;
- pointer arithmetic, pointer comparison, or general pointer casts.

These omissions are syntax errors or compile-time type errors rather than
silently accepted features.
