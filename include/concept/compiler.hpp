#pragma once

#include "concept/bytecode.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace cpt {

class CompileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Bytecode compile(std::string_view source,
                               std::string_view filename = "<input>");

} // namespace cpt
