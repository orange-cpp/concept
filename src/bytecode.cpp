#include "concept/bytecode.hpp"

#include <algorithm>
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
constexpr std::uint32_t bytecode_version = 4;
constexpr std::size_t opcode_count =
    static_cast<std::size_t>(Op::return_value) + 1;

void append_u32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& output, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
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

std::uint64_t read_u64(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        throw std::runtime_error("truncated Concept bytecode");
    }

    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
                 (index * 8);
    }
    return value;
}

std::size_t operand_size(const Op op) {
    switch (op) {
    case Op::push_bits:
        return 8;
    case Op::push_text:
        return 4;
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
    case Op::print:
    case Op::println:
        return 1;
    case Op::pop:
    case Op::input_text:
    case Op::input_i64:
    case Op::input_f64:
    case Op::return_value:
        return 0;
    }
    throw std::runtime_error("unknown Concept bytecode instruction");
}

std::uint64_t next_random(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::array<std::uint8_t, opcode_count>
make_opcode_map(const std::uint64_t seed) {
    std::array<std::uint8_t, 256> candidates{};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index] = static_cast<std::uint8_t>(index);
    }

    auto state = seed;
    for (std::size_t remaining = candidates.size(); remaining > 1;
         --remaining) {
        const auto selected =
            static_cast<std::size_t>(next_random(state) % remaining);
        std::swap(candidates[remaining - 1], candidates[selected]);
    }

    std::array<std::uint8_t, opcode_count> mapping{};
    std::copy_n(candidates.begin(), mapping.size(), mapping.begin());
    return mapping;
}

std::vector<std::uint8_t>
encode_opcodes(const std::vector<std::uint8_t>& canonical_code,
               const std::uint64_t seed) {
    const auto mapping = make_opcode_map(seed);
    auto encoded = canonical_code;
    std::size_t offset = 0;
    while (offset < canonical_code.size()) {
        const auto raw_op = canonical_code[offset];
        if (raw_op >= mapping.size()) {
            throw std::runtime_error("unknown Concept bytecode instruction");
        }
        encoded[offset] = mapping[raw_op];
        offset += 1 + operand_size(static_cast<Op>(raw_op));
    }
    return encoded;
}

std::vector<std::uint8_t>
decode_opcodes(const std::span<const std::uint8_t> encoded_code,
               const std::uint64_t seed) {
    constexpr std::uint8_t invalid_opcode = 0xff;
    std::array<std::uint8_t, 256> inverse{};
    inverse.fill(invalid_opcode);
    const auto mapping = make_opcode_map(seed);
    for (std::size_t canonical = 0; canonical < mapping.size(); ++canonical) {
        inverse[mapping[canonical]] = static_cast<std::uint8_t>(canonical);
    }

    std::vector<std::uint8_t> decoded(encoded_code.begin(), encoded_code.end());
    std::size_t offset = 0;
    while (offset < decoded.size()) {
        const auto canonical = inverse[decoded[offset]];
        if (canonical == invalid_opcode) {
            throw std::runtime_error(
                "opcode does not belong to this Concept bytecode layout");
        }
        decoded[offset] = canonical;
        const auto size = operand_size(static_cast<Op>(canonical));
        if (size > decoded.size() - offset - 1) {
            throw std::runtime_error("truncated Concept bytecode instruction");
        }
        offset += 1 + size;
    }
    return decoded;
}

} // namespace

std::vector<std::uint8_t> serialize(const Bytecode& bytecode) {
    validate(bytecode);
    if (bytecode.code.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Concept bytecode is too large");
    }
    if (bytecode.strings.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("too many strings in Concept bytecode");
    }
    std::size_t string_bytes = 0;
    for (const auto& value : bytecode.strings) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Concept string constant is too large");
        }
        string_bytes += 4 + value.size();
    }

    std::vector<std::uint8_t> output;
    const auto encoded_code =
        encode_opcodes(bytecode.code, bytecode.opcode_seed);
    output.reserve(40 + encoded_code.size() + string_bytes);
    output.insert(output.end(), bytecode_magic.begin(), bytecode_magic.end());
    append_u32(output, bytecode_version);
    append_u32(output, bytecode.entry);
    append_u32(output, bytecode.entry_locals);
    append_u32(output, static_cast<std::uint32_t>(bytecode.entry_type));
    append_u64(output, bytecode.opcode_seed);
    append_u32(output, static_cast<std::uint32_t>(encoded_code.size()));
    append_u32(output, static_cast<std::uint32_t>(bytecode.strings.size()));
    output.insert(output.end(), encoded_code.begin(), encoded_code.end());
    for (const auto& value : bytecode.strings) {
        append_u32(output, static_cast<std::uint32_t>(value.size()));
        output.insert(output.end(), value.begin(), value.end());
    }
    return output;
}

Bytecode deserialize(const std::span<const std::uint8_t> bytes) {
    constexpr std::size_t header_size = 40;
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
    if (raw_entry_type > static_cast<std::uint32_t>(ValueType::text)) {
        throw std::runtime_error("invalid Concept entry-point type");
    }
    bytecode.entry_type = static_cast<ValueType>(raw_entry_type);
    bytecode.opcode_seed = read_u64(bytes, 24);
    const auto code_size = read_u32(bytes, 32);
    const auto string_count = read_u32(bytes, 36);
    if (code_size > bytes.size() - header_size) {
        throw std::runtime_error("invalid Concept bytecode image size");
    }
    bytecode.code = decode_opcodes(bytes.subspan(header_size, code_size),
                                   bytecode.opcode_seed);
    std::size_t cursor = header_size + code_size;
    if (string_count > (bytes.size() - cursor) / 4) {
        throw std::runtime_error("invalid Concept string table size");
    }
    bytecode.strings.reserve(string_count);
    for (std::uint32_t index = 0; index < string_count; ++index) {
        const auto length = read_u32(bytes, cursor);
        cursor += 4;
        if (length > bytes.size() - cursor) {
            throw std::runtime_error("truncated Concept string constant");
        }
        bytecode.strings.emplace_back(
            reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
    }
    if (cursor != bytes.size()) {
        throw std::runtime_error("trailing data in Concept bytecode image");
    }
    validate(bytecode);
    return bytecode;
}

void validate(const Bytecode& bytecode) {
    if (bytecode.code.empty()) {
        throw std::runtime_error("Concept bytecode contains no instructions");
    }
    if (static_cast<std::uint8_t>(bytecode.entry_type) >
        static_cast<std::uint8_t>(ValueType::text)) {
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
                static_cast<std::uint8_t>(ValueType::text)) {
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

        if (op == Op::push_text) {
            const auto string_index = read_u32(bytecode.code, offset);
            if (string_index >= bytecode.strings.size()) {
                throw std::runtime_error(
                    "invalid string constant in Concept bytecode");
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
