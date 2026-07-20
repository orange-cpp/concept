#include "concept/bytecode.hpp"
#include "concept/compiler.hpp"
#include "concept/package.hpp"
#include "concept/vm.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void execution_test() {
    constexpr std::string_view source = R"(
        fn forty() -> i64 {
            return 40;
        }

        fn main() -> i64 {
            i64 value = forty();
            while (value < 42) {
                value = value + 1;
            }
            if (value == 42) {
                return value;
            } else {
                return 0;
            }
        }
    )";

    const auto compiled = cpt::compile(source, "execution-test.concept");
    expect(cpt::execute(compiled) == 42,
           "compiled control flow should evaluate to 42");

    const auto image = cpt::serialize(compiled);
    const auto loaded = cpt::deserialize(image);
    expect(cpt::execute(loaded) == 42,
           "serialized bytecode should evaluate to 42");
}

void forward_call_test() {
    constexpr std::string_view source = R"(
        fn main() -> i64 { return later(); }
        fn later() -> i64 { return 17; }
    )";
    expect(cpt::execute(cpt::compile(source)) == 17,
           "calls to functions declared later should work");
}

void randomized_opcode_test() {
    constexpr std::string_view source = R"(
        fn helper() -> i32 { return 6 * 7; }
        fn main() -> i32 {
            i32 value = helper();
            if (value == 42) { return value; }
            return 0;
        }
    )";

    const auto first = cpt::compile(source, "random-layout-a.concept");
    const auto second = cpt::compile(source, "random-layout-b.concept");
    const auto first_image = cpt::serialize(first);
    const auto second_image = cpt::serialize(second);

    expect(first.vm_seeds.size() == 4 && second.vm_seeds.size() == 4 &&
               first.vm_seeds != second.vm_seeds,
           "each compilation should receive different per-VM opcode seeds");
    expect(first_image != second_image,
           "identical source compilations should produce different bytecode");
    constexpr std::size_t header_size = 80 + 4 * 16;
    expect(first_image.size() == second_image.size() &&
               !std::equal(first_image.begin() + header_size,
                           first_image.end(),
                           second_image.begin() + header_size),
           "encoded opcode streams should use different numeric layouts");
    const auto first_loaded = cpt::deserialize(first_image);
    const auto second_loaded = cpt::deserialize(second_image);
    expect(first_loaded.vm_regions.size() == 4 &&
               second_loaded.vm_regions.size() == 4,
           "serialized programs should contain four VM bytecode regions");
    expect(cpt::execute(first_loaded) == 42 &&
               cpt::execute(second_loaded) == 42,
           "every randomized opcode layout should decode and execute");

    const auto eight_vms = cpt::deserialize(
        cpt::serialize(cpt::compile(source, "eight-vms.concept", 8)));
    expect(eight_vms.vm_regions.size() == 8 &&
               cpt::execute(eight_vms) == 42,
           "custom VM counts should preserve shared execution state");
}

void complexity_decorator_test() {
    constexpr std::string_view plain_source = R"(
        fn main() -> i32 {
            i32 answer = 6 * 7;
            return answer;
        }
    )";
    constexpr std::string_view zero_source = R"(
        @complexity(0)
        fn main() -> i32 {
            i32 answer = 6 * 7;
            return answer;
        }
    )";
    constexpr std::string_view medium_source = R"(
        @complexty(25)
        fn main() -> i32 {
            i32 answer = 6 * 7;
            return answer;
        }
    )";
    constexpr std::string_view full_source = R"(
        @complexity(100)
        fn main() -> i32 {
            i32 answer = 6 * 7;
            return answer;
        }
    )";

    const auto plain = cpt::compile(plain_source, "plain.concept", 8);
    const auto zero = cpt::compile(zero_source, "zero.concept", 8);
    const auto medium = cpt::compile(medium_source, "medium.concept", 8);
    const auto full = cpt::compile(full_source, "full.concept", 8);

    expect(plain.code == zero.code,
           "complexity zero should emit straight bytecode");
    expect(zero.code.size() < medium.code.size() &&
               medium.code.size() < full.code.size(),
           "higher complexity should emit progressively more bytecode");
    expect(cpt::execute(cpt::deserialize(cpt::serialize(medium))) == 42 &&
               cpt::execute(cpt::deserialize(cpt::serialize(full))) == 42,
           "obfuscated bytecode should preserve behavior across eight VMs");
}

void class_test() {
    constexpr std::string_view source = R"(
        class Counter {
            i32 value;
            string label;

            @complexity(25)
            fn increment() -> int {
                this.value = this.value + 1;
                return this.value;
            }

            fn increment_twice() -> int {
                this.increment();
                return this.increment();
            }

            fn get_label() -> string {
                return this.label;
            }
        }

        @complexity(50)
        fn main() -> int {
            Counter first = Counter();
            first.value = 40;
            first.label = "shared";
            Counter alias = first;
            i32 answer = alias.increment_twice();
            Counter defaults;
            if (defaults.label != "") { return 1; }
            if (first.value == 42) {
                if (alias.get_label() == "shared") { return answer; }
            }
            return 0;
        }
    )";

    const auto compiled = cpt::compile(source, "class-test.concept", 8);
    expect(cpt::execute(compiled) == 42,
           "class fields, methods, this, and aliases should execute");
    expect(cpt::execute(cpt::deserialize(cpt::serialize(compiled))) == 42,
           "class objects should survive serialized multi-VM bytecode");

    try {
        static_cast<void>(cpt::compile(R"(
            class Empty {}
            fn main() -> int {
                Empty value = Empty();
                value.missing = 1;
                return 0;
            }
        )"));
        expect(false, "unknown class fields should be compile errors");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(R"(
            class Bad { i32 value; i64 value; }
            fn main() -> int { return 0; }
        )"));
        expect(false, "duplicate class fields should be compile errors");
    } catch (const cpt::CompileError&) {
    }
}

void pointer_test() {
    constexpr std::string_view source = R"(
        class Box {
            i32 value;
            i32* alias;
        }

        @complexity(50)
        fn main() -> int {
            i32 answer = 39;
            i32* pointer = &answer;
            *pointer = *pointer + 1;
            i32** pointer_to_pointer = &pointer;
            **pointer_to_pointer = **pointer_to_pointer + 1;

            Box box;
            Box* box_pointer = &box;
            (*box_pointer).value = answer;
            i32* field_pointer = &(*box_pointer).value;
            *field_pointer = *field_pointer + 1;
            box.alias = field_pointer;

            f64 decimal = 40.5;
            f64* decimal_pointer = &decimal;
            *decimal_pointer = *decimal_pointer + 1.5;
            bool valid = true;
            bool* valid_pointer = &valid;
            string label = "pointer";
            string* label_pointer = &label;
            if (*decimal_pointer != 42.0) { return 1; }
            if (!*valid_pointer) { return 2; }
            if (*label_pointer != "pointer") { return 3; }
            return *box.alias;
        }
    )";

    const auto compiled = cpt::compile(source, "pointer-test.concept", 8);
    expect(cpt::execute(compiled) == 42,
           "local, field, class, and multi-level pointers should execute");
    expect(cpt::execute(cpt::deserialize(cpt::serialize(compiled))) == 42,
           "pointers should survive serialized multi-VM bytecode");

    constexpr std::string_view pointer_cast_source = R"(
        fn main() -> int {
            u32* pointer = ptr_cast<u32>(0x10000);
            return 42;
        }
    )";
    expect(cpt::execute(cpt::compile(pointer_cast_source,
                                     "pointer-cast-syntax.concept")) == 42,
           "ptr_cast should accept hexadecimal integral addresses");

#ifdef _WIN32
    std::uint32_t native_value = 41;
    std::ostringstream native_source;
    native_source << R"(
        fn main() -> int {
            u32* pointer = ptr_cast<u32>(0x)"
                  << std::hex
                  << reinterpret_cast<std::uintptr_t>(&native_value) << R"();
            *pointer = *pointer + 1;
            return i32(*pointer);
        }
    )";
    const auto native_bytecode =
        cpt::compile(native_source.str(), "native-pointer-test.concept", 8);
    expect(cpt::execute(cpt::deserialize(cpt::serialize(native_bytecode))) ==
               42 &&
               native_value == 42,
           "ptr_cast pointers should read and write valid process memory");
#endif

    try {
        static_cast<void>(cpt::execute(cpt::compile(R"(
            fn main() -> int {
                u32* pointer = ptr_cast<u32>(0x1);
                return i32(*pointer);
            }
        )")));
        expect(false, "unreadable native pointer addresses should fail");
    } catch (const std::runtime_error&) {
    }

    try {
        static_cast<void>(cpt::compile(R"(
            fn main() -> int {
                u32* pointer = ptr_cast<u32>(1.5);
                return 0;
            }
        )"));
        expect(false, "ptr_cast should require an integral address");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(R"(
            fn main() -> int {
                string* pointer = ptr_cast<string>(0x10000);
                return 0;
            }
        )"));
        expect(false, "native string pointers should be compile errors");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(R"(
            fn main() -> int {
                i32 value = 42;
                i64* pointer = &value;
                return 0;
            }
        )"));
        expect(false, "incompatible pointer types should be compile errors");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(R"(
            fn main() -> int {
                i32* pointer = &(40 + 2);
                return 0;
            }
        )"));
        expect(false, "taking the address of a temporary should fail");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::execute(cpt::compile(R"(
            fn main() -> int {
                i32* pointer;
                return *pointer;
            }
        )")));
        expect(false, "dereferencing a null pointer should fail at runtime");
    } catch (const std::runtime_error&) {
    }

    try {
        static_cast<void>(cpt::execute(cpt::compile(R"(
            class Holder {
                i32* pointer;
                fn capture() -> int {
                    i32 local = 42;
                    this.pointer = &local;
                    return 0;
                }
            }
            fn main() -> int {
                Holder holder;
                holder.capture();
                return *holder.pointer;
            }
        )")));
        expect(false, "dereferencing an expired local pointer should fail");
    } catch (const std::runtime_error&) {
    }
}

void import_test() {
    const auto source_root =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto standard_modules = source_root / "concept";
    const auto test_modules = source_root / "tests" / "modules";

    constexpr std::string_view custom_source = R"(
        import math;
        import math;
        fn main() -> int { return imported_answer(); }
    )";
    expect(cpt::execute(cpt::compile(custom_source, "import-test.concept", 4,
                                     test_modules.string())) == 42,
           "imports should merge a module once and expose its functions");
#ifdef _WIN32
    expect(GetModuleHandleW(L"ws2_32.dll") == nullptr,
           "programs without socket operations should not load Winsock");
#endif

    constexpr std::string_view std_source = R"(
        import std::socket;
        import std::socket;
        fn main() -> int {
            std::Socket handle = std::Socket();
            if (!handle.valid()) { return 1; }
            if (handle.close()) { return 42; }
            return 2;
        }
    )";
    expect(cpt::execute(cpt::compile(std_source, "std-import-test.concept", 8,
                                     standard_modules.string())) == 42,
           "import std::socket should enable std::Socket across multiple VMs");
#ifdef _WIN32
    expect(GetModuleHandleW(L"ws2_32.dll") != nullptr,
           "the first socket operation should load Winsock lazily");
#endif

    try {
        static_cast<void>(cpt::compile(
            "import cycle_a; fn main() -> int { return 0; }",
            "cycle-test.concept", 4, test_modules.string()));
        expect(false, "recursive module imports should be compile errors");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(
            "fn main() -> int { std::Socket value = std::Socket(); return 0; }"));
        expect(false,
               "std::Socket should require import std::socket");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(
            "import std::socket; fn main() -> int { u64 value = socket(); return 0; }",
            "old-socket-api.concept", 4, standard_modules.string()));
        expect(false, "legacy global socket functions should not be available");
    } catch (const cpt::CompileError&) {
    }
}

void console_io_test() {
    constexpr std::string_view source = R"(
        fn read_name() -> string {
            return input();
        }

        fn main() -> i32 {
            print("Name: ");
            string hidden = "This string is deliberately longer than one ChaCha20 block so encryption spans multiple blocks.";
            string name = read_name();
            println(name);
            i64 whole = input_i64();
            f64 fraction = input_f64();
            print(true);
            println(false);
            if (name == "Ada Lovelace") {
                return i32(f64(whole) + fraction);
            }
            return 0;
        }
    )";

    std::istringstream input("Ada Lovelace\n40\n2.5\n");
    std::ostringstream output;
    auto* previous_input = std::cin.rdbuf(input.rdbuf());
    auto* previous_output = std::cout.rdbuf(output.rdbuf());
    std::int64_t result = 0;
    try {
        const auto image = cpt::serialize(cpt::compile(source, "io-test.concept"));
        constexpr std::string_view secret =
            "This string is deliberately longer than one ChaCha20 block so encryption spans multiple blocks.";
        expect(std::search(image.begin(), image.end(), secret.begin(),
                           secret.end()) == image.end(),
               "packaged string constants should not appear as plaintext");
        const auto loaded = cpt::deserialize(image);
        expect(std::find(loaded.strings.begin(), loaded.strings.end(), secret) !=
                   loaded.strings.end(),
               "string constants should decrypt while loading the program");
        result = cpt::execute(loaded);
    } catch (...) {
        std::cin.rdbuf(previous_input);
        std::cout.rdbuf(previous_output);
        throw;
    }
    std::cin.rdbuf(previous_input);
    std::cout.rdbuf(previous_output);

    expect(result == 42,
           "text and numeric input should be usable by Concept programs");
    expect(output.str() == "Name: Ada Lovelace\ntruefalse\n",
           "print and println should format values and newlines correctly");
}

void core_types_test() {
    constexpr std::string_view source = R"(
        fn scaled() -> double {
            float base = 1.5;
            return f64(base) * 28.0;
        }

        fn main() -> int {
            bool yes = true;
            bool no = false;
            i8 signed8 = -8;
            i16 signed16 = signed8;
            i32 signed32 = signed16;
            i64 signed64 = signed32;
            u8 unsigned8 = 255;
            u16 unsigned16 = unsigned8;
            u32 unsigned32 = unsigned16;
            u64 unsigned64 = 18446744073709551615;
            f32 single = 1.5;
            f64 answer = scaled();
            i8 signed_wrap = 127;
            f32 quarter = f32(1.0) / f32(4.0);

            unsigned8 = unsigned8 + u8(1);
            signed_wrap = signed_wrap + i8(1);
            signed64 = signed64 + unsigned16;
            unsigned32 = u32(unsigned64);
            single = f32(answer);

            bool signed_wrapped = signed_wrap == i8(-128);
            bool quarter_is_exact = quarter == f32(0.25);
            bool numeric_bool = unsigned16;
            if (!signed_wrapped) {
                return 1;
            }
            if (!quarter_is_exact) {
                return 2;
            }
            if (!numeric_bool) {
                return 3;
            }

            if (yes) {
                if (!no) {
                    if (unsigned8 == u8(0)) {
                        if (answer == 42.0) {
                            return i32(answer);
                        }
                    }
                }
            }
            return 0;
        }
    )";

    const auto compiled = cpt::compile(source, "core-types.concept");
    expect(compiled.entry_type == cpt::ValueType::i32,
           "the entry-point type should be serialized as i32");
    expect(cpt::execute(compiled) == 42,
           "all integral, floating, bool, and alias types should execute");
    expect(cpt::execute(cpt::deserialize(cpt::serialize(compiled))) == 42,
           "typed bytecode should survive serialization");
}

void type_error_test() {
    try {
        static_cast<void>(cpt::compile(
            "fn main() -> i32 { bool value = true; return value + 1; }"));
        expect(false, "bool arithmetic should be a compile error");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(
            "fn main() -> i32 { f64 value = 4.0 % 2.0; return 0; }"));
        expect(false, "floating-point modulo should be a compile error");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile("fn main() -> f64 { return 0.0; }"));
        expect(false, "a floating-point main result should be a compile error");
    } catch (const cpt::CompileError&) {
    }
}

void error_test() {
    try {
        static_cast<void>(cpt::compile("fn main() -> i64 { return missing; }"));
        expect(false, "unknown variables should be compile errors");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile("fn nope() -> i64 { return 0; }"));
        expect(false, "a missing main function should be a compile error");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(
            "@complexity(101) fn main() -> i32 { return 0; }"));
        expect(false, "complexity above 100 should be a compile error");
    } catch (const cpt::CompileError&) {
    }

    try {
        static_cast<void>(cpt::compile(
            "@unknown(25) fn main() -> i32 { return 0; }"));
        expect(false, "unknown decorators should be compile errors");
    } catch (const cpt::CompileError&) {
    }
}

} // namespace

int main() {
    try {
        execution_test();
        forward_call_test();
        randomized_opcode_test();
        complexity_decorator_test();
        class_test();
        pointer_test();
        import_test();
        console_io_test();
        core_types_test();
        error_test();
        type_error_test();
    } catch (const std::exception& error) {
        std::cerr << "unexpected test exception: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "all Concept tests passed\n";
    return 0;
}
