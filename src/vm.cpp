#include "concept/vm.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cpt {
namespace {

struct Frame {
    std::size_t return_ip{};
    std::size_t stack_base{};
    std::vector<std::uint64_t> locals;
};

std::uint16_t read_u16(const std::vector<std::uint8_t>& code,
                       std::size_t& ip) {
    if (ip + 2 > code.size()) {
        throw std::runtime_error("VM read past the bytecode image");
    }
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(code[ip]) |
        static_cast<std::uint16_t>(code[ip + 1] << 8));
    ip += 2;
    return value;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& code,
                       std::size_t& ip) {
    if (ip + 4 > code.size()) {
        throw std::runtime_error("VM read past the bytecode image");
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(code[ip + index]) << (index * 8);
    }
    ip += 4;
    return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& code,
                       std::size_t& ip) {
    if (ip + 8 > code.size()) {
        throw std::runtime_error("VM read past the bytecode image");
    }
    std::uint64_t bits = 0;
    for (unsigned index = 0; index < 8; ++index) {
        bits |= static_cast<std::uint64_t>(code[ip + index]) << (index * 8);
    }
    ip += 8;
    return bits;
}

ValueType read_type(const std::vector<std::uint8_t>& code, std::size_t& ip) {
    if (ip >= code.size() ||
        code[ip] > static_cast<std::uint8_t>(ValueType::f64)) {
        throw std::runtime_error("VM encountered an invalid value type");
    }
    return static_cast<ValueType>(code[ip++]);
}

unsigned integral_width(const ValueType type) {
    switch (type) {
    case ValueType::i8:
    case ValueType::u8:
        return 8;
    case ValueType::i16:
    case ValueType::u16:
        return 16;
    case ValueType::i32:
    case ValueType::u32:
        return 32;
    case ValueType::i64:
    case ValueType::u64:
        return 64;
    default:
        throw std::runtime_error("VM expected an integral value type");
    }
}

std::uint64_t bit_mask(const unsigned width) {
    return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                       : (std::uint64_t{1} << width) - 1;
}

std::uint64_t normalize_integral(const ValueType type,
                                 const std::uint64_t bits) {
    return bits & bit_mask(integral_width(type));
}

std::int64_t signed_value(const ValueType type, const std::uint64_t bits) {
    const auto width = integral_width(type);
    auto normalized = normalize_integral(type, bits);
    if (width < 64 &&
        (normalized & (std::uint64_t{1} << (width - 1))) != 0) {
        normalized |= ~bit_mask(width);
    }
    return std::bit_cast<std::int64_t>(normalized);
}

float f32_value(const std::uint64_t bits) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}

double f64_value(const std::uint64_t bits) {
    return std::bit_cast<double>(bits);
}

std::uint64_t f32_bits(const float value) {
    return std::bit_cast<std::uint32_t>(value);
}

std::uint64_t f64_bits(const double value) {
    return std::bit_cast<std::uint64_t>(value);
}

bool truthy(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::boolean) {
        return bits != 0;
    }
    if (type == ValueType::f32) {
        return f32_value(bits) != 0.0F;
    }
    if (type == ValueType::f64) {
        return f64_value(bits) != 0.0;
    }
    return normalize_integral(type, bits) != 0;
}

long double numeric_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::boolean) {
        return bits == 0 ? 0.0L : 1.0L;
    }
    if (type == ValueType::f32) {
        return f32_value(bits);
    }
    if (type == ValueType::f64) {
        return f64_value(bits);
    }
    if (is_signed_integral(type)) {
        return static_cast<long double>(signed_value(type, bits));
    }
    return static_cast<long double>(normalize_integral(type, bits));
}

std::uint64_t floating_to_integral(const long double value,
                                   const ValueType target) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(
            "cannot convert a non-finite floating value to an integer");
    }

    const auto width = integral_width(target);
    const auto truncated = std::trunc(value);
    if (is_signed_integral(target)) {
        const auto magnitude = std::ldexp(1.0L, static_cast<int>(width - 1));
        if (truncated < -magnitude || truncated >= magnitude) {
            throw std::runtime_error(
                "floating value is outside the target integer range");
        }
        return normalize_integral(
            target, static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(truncated)));
    }

    const auto upper_bound = std::ldexp(1.0L, static_cast<int>(width));
    if (truncated < 0.0L || truncated >= upper_bound) {
        throw std::runtime_error(
            "floating value is outside the target integer range");
    }
    return normalize_integral(target,
                              static_cast<std::uint64_t>(truncated));
}

std::uint64_t convert_value(const std::uint64_t bits,
                            const ValueType source,
                            const ValueType target) {
    if (target == ValueType::boolean) {
        return truthy(source, bits) ? 1 : 0;
    }
    if (target == ValueType::f32) {
        return f32_bits(static_cast<float>(numeric_value(source, bits)));
    }
    if (target == ValueType::f64) {
        return f64_bits(static_cast<double>(numeric_value(source, bits)));
    }
    if (is_floating(source)) {
        return floating_to_integral(numeric_value(source, bits), target);
    }
    if (source == ValueType::boolean) {
        return normalize_integral(target, bits == 0 ? 0 : 1);
    }
    if (is_signed_integral(source)) {
        return normalize_integral(
            target,
            static_cast<std::uint64_t>(signed_value(source, bits)));
    }
    return normalize_integral(target, normalize_integral(source, bits));
}

std::uint64_t floating_arithmetic(const Op op, const ValueType type,
                                  const std::uint64_t left,
                                  const std::uint64_t right) {
    if (type == ValueType::f32) {
        const auto lhs = f32_value(left);
        const auto rhs = f32_value(right);
        switch (op) {
        case Op::add:
            return f32_bits(lhs + rhs);
        case Op::subtract:
            return f32_bits(lhs - rhs);
        case Op::multiply:
            return f32_bits(lhs * rhs);
        case Op::divide:
            return f32_bits(lhs / rhs);
        default:
            break;
        }
    } else {
        const auto lhs = f64_value(left);
        const auto rhs = f64_value(right);
        switch (op) {
        case Op::add:
            return f64_bits(lhs + rhs);
        case Op::subtract:
            return f64_bits(lhs - rhs);
        case Op::multiply:
            return f64_bits(lhs * rhs);
        case Op::divide:
            return f64_bits(lhs / rhs);
        default:
            break;
        }
    }
    throw std::runtime_error("invalid floating-point VM operation");
}

std::uint64_t integral_arithmetic(const Op op, const ValueType type,
                                  const std::uint64_t left,
                                  const std::uint64_t right) {
    const auto lhs = normalize_integral(type, left);
    const auto rhs = normalize_integral(type, right);
    switch (op) {
    case Op::add:
        return normalize_integral(type, lhs + rhs);
    case Op::subtract:
        return normalize_integral(type, lhs - rhs);
    case Op::multiply:
        return normalize_integral(type, lhs * rhs);
    case Op::divide:
    case Op::modulo:
        if (rhs == 0) {
            throw std::runtime_error(op == Op::divide
                                         ? "division by zero in Concept program"
                                         : "modulo by zero in Concept program");
        }
        if (!is_signed_integral(type)) {
            return normalize_integral(type, op == Op::divide ? lhs / rhs
                                                              : lhs % rhs);
        }
        {
            const auto signed_lhs = signed_value(type, lhs);
            const auto signed_rhs = signed_value(type, rhs);
            const auto width = integral_width(type);
            const auto minimum =
                width == 64
                    ? std::numeric_limits<std::int64_t>::min()
                    : -(std::int64_t{1} << (width - 1));
            if (signed_lhs == minimum && signed_rhs == -1) {
                return op == Op::divide ? normalize_integral(type, lhs) : 0;
            }
            const auto result = op == Op::divide ? signed_lhs / signed_rhs
                                                  : signed_lhs % signed_rhs;
            return normalize_integral(type,
                                      static_cast<std::uint64_t>(result));
        }
    default:
        throw std::runtime_error("invalid integral VM operation");
    }
}

std::uint64_t arithmetic(const Op op, const ValueType type,
                         const std::uint64_t left,
                         const std::uint64_t right) {
    return is_floating(type) ? floating_arithmetic(op, type, left, right)
                             : integral_arithmetic(op, type, left, right);
}

bool compare_values(const Op op, const ValueType type,
                    const std::uint64_t left, const std::uint64_t right) {
    if (type == ValueType::boolean) {
        const bool lhs = left != 0;
        const bool rhs = right != 0;
        return op == Op::equal ? lhs == rhs : lhs != rhs;
    }
    if (type == ValueType::f32) {
        const auto lhs = f32_value(left);
        const auto rhs = f32_value(right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else if (type == ValueType::f64) {
        const auto lhs = f64_value(left);
        const auto rhs = f64_value(right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else if (is_signed_integral(type)) {
        const auto lhs = signed_value(type, left);
        const auto rhs = signed_value(type, right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else {
        const auto lhs = normalize_integral(type, left);
        const auto rhs = normalize_integral(type, right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    }
    throw std::runtime_error("invalid comparison VM operation");
}

std::uint64_t negate_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::f32) {
        return f32_bits(-f32_value(bits));
    }
    if (type == ValueType::f64) {
        return f64_bits(-f64_value(bits));
    }
    return normalize_integral(type, std::uint64_t{0} - bits);
}

std::int64_t exit_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::boolean) {
        return bits == 0 ? 0 : 1;
    }
    if (is_signed_integral(type)) {
        return signed_value(type, bits);
    }
    if (is_integral(type)) {
        const auto value = normalize_integral(type, bits);
        return type == ValueType::u64 ? std::bit_cast<std::int64_t>(value)
                                      : static_cast<std::int64_t>(value);
    }
    throw std::runtime_error(
        "Concept entry point returned a floating-point value");
}

} // namespace

std::int64_t execute(const Bytecode& bytecode) {
    validate(bytecode);

    constexpr std::size_t maximum_call_depth = 4096;
    std::vector<std::uint64_t> stack;
    std::vector<Frame> frames;
    frames.push_back(
        {bytecode.code.size(), 0,
         std::vector<std::uint64_t>(bytecode.entry_locals, 0)});
    std::size_t ip = bytecode.entry;

    const auto pop = [&]() {
        if (stack.empty() || stack.size() <= frames.back().stack_base) {
            throw std::runtime_error("Concept VM operand stack underflow");
        }
        const auto value = stack.back();
        stack.pop_back();
        return value;
    };

    for (;;) {
        if (ip >= bytecode.code.size()) {
            throw std::runtime_error("Concept VM instruction pointer is invalid");
        }
        const auto op = static_cast<Op>(bytecode.code[ip++]);

        switch (op) {
        case Op::push_bits:
            stack.push_back(read_u64(bytecode.code, ip));
            break;
        case Op::load: {
            const auto index = read_u16(bytecode.code, ip);
            if (index >= frames.back().locals.size()) {
                throw std::runtime_error("Concept VM local load is out of range");
            }
            stack.push_back(frames.back().locals[index]);
            break;
        }
        case Op::store: {
            const auto index = read_u16(bytecode.code, ip);
            const auto value = pop();
            if (index >= frames.back().locals.size()) {
                throw std::runtime_error("Concept VM local store is out of range");
            }
            frames.back().locals[index] = value;
            break;
        }
        case Op::pop:
            static_cast<void>(pop());
            break;
        case Op::convert: {
            const auto source = read_type(bytecode.code, ip);
            const auto target = read_type(bytecode.code, ip);
            stack.push_back(convert_value(pop(), source, target));
            break;
        }
        case Op::add:
        case Op::subtract:
        case Op::multiply:
        case Op::divide:
        case Op::modulo: {
            const auto type = read_type(bytecode.code, ip);
            const auto right = pop();
            const auto left = pop();
            stack.push_back(arithmetic(op, type, left, right));
            break;
        }
        case Op::negate: {
            const auto type = read_type(bytecode.code, ip);
            stack.push_back(negate_value(type, pop()));
            break;
        }
        case Op::logical_not: {
            const auto type = read_type(bytecode.code, ip);
            stack.push_back(truthy(type, pop()) ? 0 : 1);
            break;
        }
        case Op::equal:
        case Op::not_equal:
        case Op::less:
        case Op::less_equal:
        case Op::greater:
        case Op::greater_equal: {
            const auto type = read_type(bytecode.code, ip);
            const auto right = pop();
            const auto left = pop();
            stack.push_back(compare_values(op, type, left, right) ? 1 : 0);
            break;
        }
        case Op::jump:
            ip = read_u32(bytecode.code, ip);
            break;
        case Op::jump_if_false: {
            const auto target = read_u32(bytecode.code, ip);
            if (pop() == 0) {
                ip = target;
            }
            break;
        }
        case Op::call: {
            const auto target = read_u32(bytecode.code, ip);
            const auto local_count = read_u32(bytecode.code, ip);
            if (frames.size() >= maximum_call_depth) {
                throw std::runtime_error("Concept VM call stack overflow");
            }
            frames.push_back(
                {ip, stack.size(), std::vector<std::uint64_t>(local_count, 0)});
            ip = target;
            break;
        }
        case Op::return_value: {
            const auto result = pop();
            const auto stack_base = frames.back().stack_base;
            const auto return_ip = frames.back().return_ip;
            stack.resize(stack_base);
            frames.pop_back();
            if (frames.empty()) {
                return exit_value(bytecode.entry_type, result);
            }
            stack.push_back(result);
            ip = return_ip;
            break;
        }
        default:
            throw std::runtime_error("Concept VM encountered an invalid opcode");
        }
    }
}

} // namespace cpt
