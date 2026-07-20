#include "concept/bytecode.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cpt {
namespace {

constexpr std::array<std::uint8_t, 8> bytecode_magic{
    'C', 'O', 'N', 'C', 'E', 'P', 'T', 0,
};
constexpr std::uint32_t bytecode_version = 2;

void append_u32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated Concept bytecode");
    }

    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

std::size_t operand_size(const Op op) {
    switch (op) {
    case Op::push_bits:
        return 8;
    case Op::load:
    case Op::store:
        return 2;
    case Op::convert:
        return 2;
    case Op::jump:
    case Op::jump_if_false:
        return 4;
    case Op::call:
        return 8;
    case Op::add:
    case Op::subtract:
    case Op::multiply:
    case Op::divide:
    case Op::modulo:
    case Op::negate:
    case Op::logical_not:
    case Op::equal:
    case Op::not_equal:
    case Op::less:
    case Op::less_equal:
    case Op::greater:
    case Op::greater_equal:
        return 1;
    case Op::pop:
    case Op::return_value:
        return 0;
    }
    throw std::runtime_error("unknown Concept bytecode instruction");
}

} // namespace

std::vector<std::uint8_t> serialize(const Bytecode& bytecode) {
    validate(bytecode);
    if (bytecode.code.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Concept bytecode is too large");
    }

    std::vector<std::uint8_t> output;
    output.reserve(28 + bytecode.code.size());
    output.insert(output.end(), bytecode_magic.begin(), bytecode_magic.end());
    append_u32(output, bytecode_version);
    append_u32(output, bytecode.entry);
    append_u32(output, bytecode.entry_locals);
    append_u32(output, static_cast<std::uint32_t>(bytecode.entry_type));
    append_u32(output, static_cast<std::uint32_t>(bytecode.code.size()));
    output.insert(output.end(), bytecode.code.begin(), bytecode.code.end());
    return output;
}

Bytecode deserialize(const std::span<const std::uint8_t> bytes) {
    constexpr std::size_t header_size = 28;
    if (bytes.size() < header_size ||
        !std::equal(bytecode_magic.begin(), bytecode_magic.end(), bytes.begin())) {
        throw std::runtime_error("not a Concept bytecode image");
    }

    const auto version = read_u32(bytes, 8);
    if (version != bytecode_version) {
        throw std::runtime_error("unsupported Concept bytecode version " +
                                 std::to_string(version));
    }

    Bytecode bytecode;
    bytecode.entry = read_u32(bytes, 12);
    bytecode.entry_locals = read_u32(bytes, 16);
    const auto raw_entry_type = read_u32(bytes, 20);
    if (raw_entry_type > static_cast<std::uint32_t>(ValueType::f64)) {
        throw std::runtime_error("invalid Concept entry-point type");
    }
    bytecode.entry_type = static_cast<ValueType>(raw_entry_type);
    const auto code_size = read_u32(bytes, 24);
    if (bytes.size() != header_size + code_size) {
        throw std::runtime_error("invalid Concept bytecode image size");
    }
    bytecode.code.assign(bytes.begin() + header_size, bytes.end());
    validate(bytecode);
    return bytecode;
}

void validate(const Bytecode& bytecode) {
    if (bytecode.code.empty()) {
        throw std::runtime_error("Concept bytecode contains no instructions");
    }
    if (static_cast<std::uint8_t>(bytecode.entry_type) >
        static_cast<std::uint8_t>(ValueType::f64)) {
        throw std::runtime_error("invalid Concept entry-point type");
    }

    std::vector<bool> instruction_starts(bytecode.code.size(), false);
    std::vector<std::uint32_t> targets;
    std::size_t offset = 0;
    while (offset < bytecode.code.size()) {
        instruction_starts[offset] = true;
        const auto raw_op = bytecode.code[offset++];
        if (raw_op > static_cast<std::uint8_t>(Op::return_value)) {
            throw std::runtime_error("unknown Concept bytecode instruction");
        }

        const auto op = static_cast<Op>(raw_op);
        const auto size = operand_size(op);
        if (size > bytecode.code.size() - offset) {
            throw std::runtime_error("truncated Concept bytecode instruction");
        }

        const auto check_type = [&](const std::size_t type_offset) {
            if (bytecode.code[type_offset] >
                static_cast<std::uint8_t>(ValueType::f64)) {
                throw std::runtime_error("invalid value type in Concept bytecode");
            }
            return static_cast<ValueType>(bytecode.code[type_offset]);
        };

        if (op == Op::convert) {
            static_cast<void>(check_type(offset));
            static_cast<void>(check_type(offset + 1));
        } else if (size == 1) {
            const auto type = check_type(offset);
            if ((op == Op::add || op == Op::subtract ||
                 op == Op::multiply || op == Op::divide ||
                 op == Op::negate || op == Op::less ||
                 op == Op::less_equal || op == Op::greater ||
                 op == Op::greater_equal) &&
                !is_numeric(type)) {
                throw std::runtime_error(
                    "non-numeric instruction type in Concept bytecode");
            }
            if (op == Op::modulo && !is_integral(type)) {
                throw std::runtime_error(
                    "non-integral modulo type in Concept bytecode");
            }
        }

        if (op == Op::jump || op == Op::jump_if_false || op == Op::call) {
            targets.push_back(read_u32(bytecode.code, offset));
        }
        offset += size;
    }

    const auto check_target = [&](const std::uint32_t target,
                                  const char* description) {
        if (target >= instruction_starts.size() || !instruction_starts[target]) {
            throw std::runtime_error(std::string("invalid ") + description +
                                     " in Concept bytecode");
        }
    };

    check_target(bytecode.entry, "entry point");
    for (const auto target : targets) {
        check_target(target, "branch target");
    }
}

} // namespace cpt
