#pragma once

#include "concept/bytecode.hpp"

#include <cstdint>

namespace cpt {

[[nodiscard]] std::int64_t execute(const Bytecode& bytecode);

} // namespace cpt
