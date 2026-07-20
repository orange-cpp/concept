#include "concept/bytecode.hpp"

#include <xorstr.hpp>

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
constexpr std::uint32_t bytecode_version = 11;
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
        throw std::runtime_error(xorstr_("truncated Concept bytecode"));
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
        throw std::runtime_error(xorstr_("truncated Concept bytecode"));
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
    case Op::new_object:
    case Op::load_field:
    case Op::store_field:
    case Op::address_local:
    case Op::address_field:
        return 2;
    case Op::convert:
        return 2;
    case Op::jump:
    case Op::jump_if_false:
        return 4;
    case Op::call:
    case Op::call_method:
    case Op::call_constructor:
        return 10;
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
    case Op::native_pointer:
    case Op::heap_alloc:
    case Op::pointer_offset:
        return 1;
    case Op::array_alloc:
        return 5;
    case Op::pop:
    case Op::heap_free:
    case Op::load_indirect:
    case Op::store_indirect:
    case Op::input_text:
    case Op::input_i64:
    case Op::input_f64:
    case Op::socket_open:
    case Op::socket_connect:
    case Op::socket_bind:
    case Op::socket_listen:
    case Op::socket_accept:
    case Op::socket_send:
    case Op::socket_receive:
    case Op::socket_close:
    case Op::return_value:
        return 0;
    }
    throw std::runtime_error(
        xorstr_("unknown Concept bytecode instruction"));
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

std::vector<Bytecode::VmRegion>
make_vm_regions(const std::vector<std::uint8_t>& canonical_code,
                const std::vector<std::uint64_t>& requested_seeds) {
    std::vector<std::uint32_t> instruction_starts;
    std::size_t offset = 0;
    while (offset < canonical_code.size()) {
        instruction_starts.push_back(static_cast<std::uint32_t>(offset));
        const auto raw_op = canonical_code[offset];
        if (raw_op > static_cast<std::uint8_t>(Op::return_value)) {
            throw std::runtime_error(
                xorstr_("unknown Concept bytecode instruction"));
        }
        offset += 1 + operand_size(static_cast<Op>(raw_op));
        if (offset > canonical_code.size()) {
            throw std::runtime_error(
                xorstr_("truncated Concept bytecode instruction"));
        }
    }

    const std::size_t requested_count =
        requested_seeds.empty() ? 1 : requested_seeds.size();
    const auto region_count =
        std::min(requested_count, instruction_starts.size());
    std::vector<Bytecode::VmRegion> regions;
    regions.reserve(region_count);
    for (std::size_t region = 0; region < region_count; ++region) {
        const auto first_instruction =
            region * instruction_starts.size() / region_count;
        const auto next_instruction =
            (region + 1) * instruction_starts.size() / region_count;
        regions.push_back(
            {instruction_starts[first_instruction],
             next_instruction == instruction_starts.size()
                 ? static_cast<std::uint32_t>(canonical_code.size())
                 : instruction_starts[next_instruction],
             requested_seeds.empty() ? 0 : requested_seeds[region]});
    }
    return regions;
}

std::vector<std::uint8_t> encode_opcodes(
    const std::vector<std::uint8_t>& canonical_code,
    const std::vector<Bytecode::VmRegion>& regions) {
    auto encoded = canonical_code;
    for (const auto& region : regions) {
        const auto mapping = make_opcode_map(region.opcode_seed);
        std::size_t offset = region.begin;
        while (offset < region.end) {
            const auto raw_op = canonical_code[offset];
            if (raw_op >= mapping.size()) {
                throw std::runtime_error(
                    xorstr_("unknown Concept bytecode instruction"));
            }
            encoded[offset] = mapping[raw_op];
            offset += 1 + operand_size(static_cast<Op>(raw_op));
        }
        if (offset != region.end) {
            throw std::runtime_error(
                xorstr_("VM region splits a bytecode instruction"));
        }
    }
    return encoded;
}

std::vector<std::uint8_t> decode_opcodes(
    const std::span<const std::uint8_t> encoded_code,
    const std::vector<Bytecode::VmRegion>& regions) {
    constexpr std::uint8_t invalid_opcode = 0xff;
    std::vector<std::uint8_t> decoded(encoded_code.begin(), encoded_code.end());
    for (const auto& region : regions) {
        std::array<std::uint8_t, 256> inverse{};
        inverse.fill(invalid_opcode);
        const auto mapping = make_opcode_map(region.opcode_seed);
        for (std::size_t canonical = 0; canonical < mapping.size(); ++canonical) {
            inverse[mapping[canonical]] = static_cast<std::uint8_t>(canonical);
        }

        std::size_t offset = region.begin;
        while (offset < region.end) {
            const auto canonical = inverse[decoded[offset]];
            if (canonical == invalid_opcode) {
                throw std::runtime_error(xorstr_(
                    "opcode does not belong to its Concept VM context"));
            }
            decoded[offset] = canonical;
            const auto size = operand_size(static_cast<Op>(canonical));
            if (size > region.end - offset - 1) {
                throw std::runtime_error(xorstr_(
                    "VM region splits a bytecode instruction"));
            }
            offset += 1 + size;
        }
        if (offset != region.end) {
            throw std::runtime_error(
                xorstr_("invalid Concept VM region boundary"));
        }
    }
    return decoded;
}

std::uint32_t load_word(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void quarter_round(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c,
                   std::uint32_t& d) {
    a += b;
    d ^= a;
    d = std::rotl(d, 16);
    c += d;
    b ^= c;
    b = std::rotl(b, 12);
    a += b;
    d ^= a;
    d = std::rotl(d, 8);
    c += d;
    b ^= c;
    b = std::rotl(b, 7);
}

std::array<std::uint8_t, 64> chacha20_block(
    const std::array<std::uint8_t, 32>& key,
    const std::array<std::uint8_t, 12>& nonce,
    const std::uint32_t counter) {
    std::array<std::uint32_t, 16> state{
        0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
    };
    for (std::size_t index = 0; index < 8; ++index) {
        state[4 + index] = load_word(key.data() + index * 4);
    }
    state[12] = counter;
    state[13] = load_word(nonce.data());
    state[14] = load_word(nonce.data() + 4);
    state[15] = load_word(nonce.data() + 8);

    auto working = state;
    for (unsigned round = 0; round < 10; ++round) {
        quarter_round(working[0], working[4], working[8], working[12]);
        quarter_round(working[1], working[5], working[9], working[13]);
        quarter_round(working[2], working[6], working[10], working[14]);
        quarter_round(working[3], working[7], working[11], working[15]);
        quarter_round(working[0], working[5], working[10], working[15]);
        quarter_round(working[1], working[6], working[11], working[12]);
        quarter_round(working[2], working[7], working[8], working[13]);
        quarter_round(working[3], working[4], working[9], working[14]);
    }

    std::array<std::uint8_t, 64> block{};
    for (std::size_t word = 0; word < working.size(); ++word) {
        const auto value = working[word] + state[word];
        for (unsigned byte = 0; byte < 4; ++byte) {
            block[word * 4 + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8));
        }
    }
    return block;
}

void crypt_string(std::span<std::uint8_t> bytes,
                  const std::array<std::uint8_t, 32>& key,
                  const std::array<std::uint8_t, 12>& nonce,
                  std::uint32_t counter) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto block = chacha20_block(key, nonce, counter++);
        const auto count = std::min(block.size(), bytes.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            bytes[offset + index] ^= block[index];
        }
        offset += count;
    }
}

void crypt_string_at_index(
    std::span<std::uint8_t> bytes,
    const std::array<std::uint8_t, 32>& key,
    const std::array<std::uint8_t, 12>& base_nonce,
    const std::uint32_t string_index) {
    auto nonce = base_nonce;
    std::uint64_t carry = string_index;
    for (std::size_t index = 0; index < nonce.size() && carry != 0; ++index) {
        const auto sum = static_cast<std::uint64_t>(nonce[index]) +
                         (carry & 0xffU);
        nonce[index] = static_cast<std::uint8_t>(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
    crypt_string(bytes, key, nonce, 1);
}

} // namespace

std::vector<std::uint8_t> serialize(const Bytecode& bytecode) {
    validate(bytecode);
    if (bytecode.code.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(xorstr_("Concept bytecode is too large"));
    }
    if (bytecode.strings.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            xorstr_("too many strings in Concept bytecode"));
    }
    std::size_t string_bytes = 0;
    for (const auto& value : bytecode.strings) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                xorstr_("Concept string constant is too large"));
        }
        string_bytes += 4 + value.size();
    }

    const auto regions = make_vm_regions(bytecode.code, bytecode.vm_seeds);
    if (regions.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            xorstr_("too many VM regions in Concept bytecode"));
    }
    const auto encoded_code = encode_opcodes(bytecode.code, regions);

    constexpr std::size_t header_size = 80;
    constexpr std::size_t region_size = 16;
    std::vector<std::uint8_t> output;
    output.reserve(header_size + regions.size() * region_size +
                   encoded_code.size() + string_bytes);
    output.insert(output.end(), bytecode_magic.begin(), bytecode_magic.end());
    append_u32(output, bytecode_version);
    append_u32(output, bytecode.entry);
    append_u32(output, bytecode.entry_locals);
    append_u32(output, static_cast<std::uint32_t>(bytecode.entry_type));
    append_u32(output, static_cast<std::uint32_t>(encoded_code.size()));
    append_u32(output, static_cast<std::uint32_t>(bytecode.strings.size()));
    append_u32(output, static_cast<std::uint32_t>(regions.size()));
    output.insert(output.end(), bytecode.string_key.begin(),
                  bytecode.string_key.end());
    output.insert(output.end(), bytecode.string_nonce.begin(),
                  bytecode.string_nonce.end());
    for (const auto& region : regions) {
        append_u32(output, region.begin);
        append_u32(output, region.end);
        append_u64(output, region.opcode_seed);
    }
    output.insert(output.end(), encoded_code.begin(), encoded_code.end());
    for (std::size_t index = 0; index < bytecode.strings.size(); ++index) {
        const auto& value = bytecode.strings[index];
        append_u32(output, static_cast<std::uint32_t>(value.size()));
        std::vector<std::uint8_t> encrypted(value.begin(), value.end());
        crypt_string_at_index(encrypted, bytecode.string_key,
                              bytecode.string_nonce,
                              static_cast<std::uint32_t>(index));
        output.insert(output.end(), encrypted.begin(), encrypted.end());
    }
    return output;
}

Bytecode deserialize(const std::span<const std::uint8_t> bytes) {
    constexpr std::size_t header_size = 80;
    constexpr std::size_t region_size = 16;
    if (bytes.size() < header_size ||
        !std::equal(bytecode_magic.begin(), bytecode_magic.end(), bytes.begin())) {
        throw std::runtime_error(xorstr_("not a Concept bytecode image"));
    }

    const auto version = read_u32(bytes, 8);
    if (version != bytecode_version) {
        throw std::runtime_error(
            std::string(xorstr_("unsupported Concept bytecode version ")) +
            std::to_string(version));
    }

    Bytecode bytecode;
    bytecode.entry = read_u32(bytes, 12);
    bytecode.entry_locals = read_u32(bytes, 16);
    const auto raw_entry_type = read_u32(bytes, 20);
    if (raw_entry_type > static_cast<std::uint32_t>(ValueType::text)) {
        throw std::runtime_error(
            xorstr_("invalid Concept entry-point type"));
    }
    bytecode.entry_type = static_cast<ValueType>(raw_entry_type);
    const auto code_size = read_u32(bytes, 24);
    const auto string_count = read_u32(bytes, 28);
    const auto vm_count = read_u32(bytes, 32);
    std::copy_n(bytes.begin() + 36, bytecode.string_key.size(),
                bytecode.string_key.begin());
    std::copy_n(bytes.begin() + 68, bytecode.string_nonce.size(),
                bytecode.string_nonce.begin());

    std::size_t cursor = header_size;
    if (vm_count == 0 || vm_count > (bytes.size() - cursor) / region_size) {
        throw std::runtime_error(
            xorstr_("invalid Concept VM region table"));
    }
    bytecode.vm_regions.reserve(vm_count);
    bytecode.vm_seeds.reserve(vm_count);
    std::uint32_t previous_end = 0;
    for (std::uint32_t index = 0; index < vm_count; ++index) {
        const auto begin = read_u32(bytes, cursor);
        const auto end = read_u32(bytes, cursor + 4);
        const auto seed = read_u64(bytes, cursor + 8);
        cursor += region_size;
        if (begin != previous_end || end <= begin || end > code_size) {
            throw std::runtime_error(
                xorstr_("invalid Concept VM region boundary"));
        }
        bytecode.vm_regions.push_back({begin, end, seed});
        bytecode.vm_seeds.push_back(seed);
        previous_end = end;
    }
    if (previous_end != code_size || code_size > bytes.size() - cursor) {
        throw std::runtime_error(
            xorstr_("invalid Concept bytecode image size"));
    }
    bytecode.code = decode_opcodes(bytes.subspan(cursor, code_size),
                                   bytecode.vm_regions);
    cursor += code_size;
    if (string_count > (bytes.size() - cursor) / 4) {
        throw std::runtime_error(
            xorstr_("invalid Concept string table size"));
    }
    bytecode.strings.reserve(string_count);
    for (std::uint32_t index = 0; index < string_count; ++index) {
        const auto length = read_u32(bytes, cursor);
        cursor += 4;
        if (length > bytes.size() - cursor) {
            throw std::runtime_error(
                xorstr_("truncated Concept string constant"));
        }
        std::vector<std::uint8_t> decrypted(
            bytes.begin() + cursor, bytes.begin() + cursor + length);
        crypt_string_at_index(decrypted, bytecode.string_key,
                              bytecode.string_nonce, index);
        bytecode.strings.emplace_back(decrypted.begin(), decrypted.end());
        cursor += length;
    }
    if (cursor != bytes.size()) {
        throw std::runtime_error(
            xorstr_("trailing data in Concept bytecode image"));
    }
    validate(bytecode);
    return bytecode;
}

void validate(const Bytecode& bytecode) {
    if (bytecode.code.empty()) {
        throw std::runtime_error(
            xorstr_("Concept bytecode contains no instructions"));
    }
    if (static_cast<std::uint8_t>(bytecode.entry_type) >
        static_cast<std::uint8_t>(ValueType::text)) {
        throw std::runtime_error(
            xorstr_("invalid Concept entry-point type"));
    }

    std::vector<bool> instruction_starts(bytecode.code.size(), false);
    std::vector<std::uint32_t> targets;
    std::size_t offset = 0;
    while (offset < bytecode.code.size()) {
        instruction_starts[offset] = true;
        const auto raw_op = bytecode.code[offset++];
        if (raw_op > static_cast<std::uint8_t>(Op::return_value)) {
            throw std::runtime_error(
                xorstr_("unknown Concept bytecode instruction"));
        }

        const auto op = static_cast<Op>(raw_op);
        const auto size = operand_size(op);
        if (size > bytecode.code.size() - offset) {
            throw std::runtime_error(
                xorstr_("truncated Concept bytecode instruction"));
        }

        const auto check_type = [&](const std::size_t type_offset) {
            if (bytecode.code[type_offset] >
                static_cast<std::uint8_t>(ValueType::text)) {
                throw std::runtime_error(
                    xorstr_("invalid value type in Concept bytecode"));
            }
            return static_cast<ValueType>(bytecode.code[type_offset]);
        };

        if (op == Op::convert) {
            static_cast<void>(check_type(offset));
            static_cast<void>(check_type(offset + 1));
        } else if (size == 1 || op == Op::array_alloc) {
            const auto type = check_type(offset);
            if (op == Op::native_pointer && type == ValueType::text) {
                throw std::runtime_error(
                    xorstr_("native string pointer in Concept bytecode"));
            }
            if ((op == Op::add || op == Op::subtract ||
                 op == Op::multiply || op == Op::divide ||
                 op == Op::negate || op == Op::less ||
                 op == Op::less_equal || op == Op::greater ||
                 op == Op::greater_equal) &&
                !is_numeric(type)) {
                throw std::runtime_error(xorstr_(
                    "non-numeric instruction type in Concept bytecode"));
            }
            if (op == Op::modulo && !is_integral(type)) {
                throw std::runtime_error(xorstr_(
                    "non-integral modulo type in Concept bytecode"));
            }
        }

        if (op == Op::push_text) {
            const auto string_index = read_u32(bytecode.code, offset);
            if (string_index >= bytecode.strings.size()) {
                throw std::runtime_error(xorstr_(
                    "invalid string constant in Concept bytecode"));
            }
        }

        if (op == Op::jump || op == Op::jump_if_false || op == Op::call ||
            op == Op::call_method || op == Op::call_constructor) {
            targets.push_back(read_u32(bytecode.code, offset));
        }
        offset += size;
    }

    if (!bytecode.vm_regions.empty()) {
        std::uint32_t previous_end = 0;
        for (const auto& region : bytecode.vm_regions) {
            if (region.begin != previous_end || region.end <= region.begin ||
                region.end > instruction_starts.size() ||
                !instruction_starts[region.begin] ||
                (region.end != instruction_starts.size() &&
                 !instruction_starts[region.end])) {
                throw std::runtime_error(
                    xorstr_("invalid Concept VM region layout"));
            }
            previous_end = region.end;
        }
        if (previous_end != instruction_starts.size()) {
            throw std::runtime_error(xorstr_(
                "Concept VM regions do not cover bytecode"));
        }
    }

    const auto check_target = [&](const std::uint32_t target,
                                  const char* description) {
        if (target >= instruction_starts.size() || !instruction_starts[target]) {
            throw std::runtime_error(
                std::string(xorstr_("invalid ")) + description +
                xorstr_(" in Concept bytecode"));
        }
    };

    check_target(bytecode.entry, xorstr_("entry point"));
    for (const auto target : targets) {
        check_target(target, xorstr_("branch target"));
    }
}

} // namespace cpt
