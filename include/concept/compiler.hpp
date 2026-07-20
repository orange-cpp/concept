#pragma once

#include "concept/bytecode.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace cpt {

enum class CompileMode {
    executable,
    shared_module,
};

class CompileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Bytecode compile(std::string_view source,
                               std::string_view filename = "<input>",
                               std::uint32_t vm_count = 4,
                               std::string_view module_root = {},
                               CompileMode mode = CompileMode::executable);

} // namespace cpt
