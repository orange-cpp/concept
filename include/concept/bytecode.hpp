#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace cpt {

enum class ValueType : std::uint8_t {
    boolean,
    i8,
    i16,
    i32,
    i64,
    u8,
    u16,
    u32,
    u64,
    f32,
    f64,
    text,
    void_type,
};

[[nodiscard]] constexpr bool is_integral(const ValueType type) {
    return type >= ValueType::i8 && type <= ValueType::u64;
}

[[nodiscard]] constexpr bool is_signed_integral(const ValueType type) {
    return type >= ValueType::i8 && type <= ValueType::i64;
}

[[nodiscard]] constexpr bool is_floating(const ValueType type) {
    return type == ValueType::f32 || type == ValueType::f64;
}

[[nodiscard]] constexpr bool is_numeric(const ValueType type) {
    return is_integral(type) || is_floating(type);
}

enum class Op : std::uint8_t {
    push_bits,
    push_text,
    load,
    store,
    pop,
    convert,
    add,
    subtract,
    multiply,
    divide,
    modulo,
    bit_and,
    bit_or,
    bit_xor,
    shift_left,
    shift_right,
    bit_not,
    negate,
    logical_not,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    jump,
    jump_if_false,
    call,
    call_method,
    call_constructor,
    new_object,
    load_field,
    store_field,
    address_local,
    address_field,
    native_pointer,
    array_alloc,
    heap_alloc,
    heap_free,
    pointer_offset,
    load_indirect,
    store_indirect,
    input_text,
    input_i64,
    input_f64,
    entropy_fill,
    text_length,
    text_byte,
    text_from_bytes,
    system_verify_x509,
    print,
    println,
    native_call,
    socket_open,
    socket_connect,
    socket_bind,
    socket_listen,
    socket_accept,
    socket_send,
    socket_receive,
    socket_send_bytes,
    socket_receive_bytes,
    socket_close,
    return_value,
};

struct Bytecode {
    struct VmRegion {
        std::uint32_t begin{};
        std::uint32_t end{};
        std::uint64_t opcode_seed{};
    };

    std::vector<std::uint8_t> code;
    std::uint32_t entry{};
    std::uint32_t entry_locals{};
    ValueType entry_type{ValueType::i64};
    std::vector<std::uint64_t> vm_seeds;
    std::vector<VmRegion> vm_regions;
    std::array<std::uint8_t, 32> string_key{};
    std::array<std::uint8_t, 12> string_nonce{};
    std::vector<std::string> strings;
};

[[nodiscard]] std::vector<std::uint8_t> serialize(const Bytecode& bytecode);
[[nodiscard]] Bytecode deserialize(std::span<const std::uint8_t> bytes);
void validate(const Bytecode& bytecode);

} // namespace cpt
