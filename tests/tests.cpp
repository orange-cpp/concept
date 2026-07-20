#include "concept/bytecode.hpp"
#include "concept/compiler.hpp"
#include "concept/package.hpp"
#include "concept/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

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
}

} // namespace

int main() {
    try {
        execution_test();
        forward_call_test();
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
