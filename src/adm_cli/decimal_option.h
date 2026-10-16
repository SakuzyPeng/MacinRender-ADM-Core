#pragma once

#include <limits>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "decimal.h"

namespace mradm::cli {

// A floating-point option parsed by the project's locale-independent, correctly rounded parser
// (PCM consistency 3.5) instead of CLI11's strtold, whose long double differs by platform; float
// targets are rounded once from the decimal text. The inclusive range is checked on the result.
template <typename T>
CLI::Option* add_decimal_option(CLI::App& app,
                                const std::string& name,
                                T& target,
                                const std::string& description,
                                T minimum = std::numeric_limits<T>::lowest(),
                                T maximum = std::numeric_limits<T>::max()) {
    auto* option = app.add_option_function<std::string>(
        name,
        [&target, name, minimum, maximum](const std::string& text) {
            const auto value = dsp::parse_decimal<T>(text);
            if (!value) {
                throw CLI::ValidationError(name, fmt::format("'{}' is not a finite decimal number", text));
            }
            if (*value < minimum || *value > maximum) {
                throw CLI::ValidationError(name, fmt::format("{} is not in range [{} - {}]", text, minimum, maximum));
            }
            target = *value;
        },
        description);
    return option->type_name("FLOAT");
}

} // namespace mradm::cli
