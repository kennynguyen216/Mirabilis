#pragma once

#include <string_view>

// Boolean environment variables, parsed one way everywhere.  Presence used to
// be the whole test, so a capture script that wrote MIRABILIS_LUMEN_LITE=0 to
// turn the feature off turned it on instead, and every measurement taken that
// way described the wrong configuration.  Unsetting the variable is no longer
// the only way to say "off".
enum class EnvBool {
    Off,      // absent, empty, 0, false, off, no
    On,       // 1, true, on, yes
    Invalid,  // anything else: the caller warns and treats it as disabled
};

namespace env_flags_detail {

constexpr bool equals_ignoring_case(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
        if (left != right) {
            return false;
        }
    }
    return true;
}

// `set VAR=1 ` in a batch file keeps the trailing space.  Trimming costs four
// lines and stops that from reading as an invalid value, which would silently
// leave a feature off in a run the author believed had it on.
constexpr std::string_view trim(std::string_view text)
{
    constexpr std::string_view Blank = " \t\r\n";
    const std::size_t first = text.find_first_not_of(Blank);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(Blank) - first + 1);
}

}  // namespace env_flags_detail

// `value` is what getenv returned: null when the variable is absent.  Absent
// and empty are both disabled, per the R1 contract: `set VAR=` is how a batch
// file clears a variable, and it must not read as "leave whatever was there".
constexpr EnvBool parse_env_bool(const char* value)
{
    if (value == nullptr) {
        return EnvBool::Off;
    }
    const std::string_view text = env_flags_detail::trim(value);
    if (text.empty()) {
        return EnvBool::Off;
    }
    for (std::string_view off : {"0", "false", "off", "no"}) {
        if (env_flags_detail::equals_ignoring_case(text, off)) {
            return EnvBool::Off;
        }
    }
    for (std::string_view on : {"1", "true", "on", "yes"}) {
        if (env_flags_detail::equals_ignoring_case(text, on)) {
            return EnvBool::On;
        }
    }
    return EnvBool::Invalid;
}

// The truth table is checked by the compiler, so a Debug or Release build is
// the test run.  Nothing here touches the GPU, so there is nothing a runtime
// test could add.
static_assert(parse_env_bool(nullptr) == EnvBool::Off);
static_assert(parse_env_bool("") == EnvBool::Off);
static_assert(parse_env_bool("   ") == EnvBool::Off);
static_assert(parse_env_bool("0") == EnvBool::Off);
static_assert(parse_env_bool("false") == EnvBool::Off);
static_assert(parse_env_bool("FALSE") == EnvBool::Off);
static_assert(parse_env_bool("Off") == EnvBool::Off);
static_assert(parse_env_bool("no") == EnvBool::Off);
static_assert(parse_env_bool(" 0 ") == EnvBool::Off);
static_assert(parse_env_bool("1") == EnvBool::On);
static_assert(parse_env_bool("true") == EnvBool::On);
static_assert(parse_env_bool("TRUE") == EnvBool::On);
static_assert(parse_env_bool("On") == EnvBool::On);
static_assert(parse_env_bool("yes") == EnvBool::On);
static_assert(parse_env_bool("\t1\r\n") == EnvBool::On);
static_assert(parse_env_bool("2") == EnvBool::Invalid);
static_assert(parse_env_bool("01") == EnvBool::Invalid);
static_assert(parse_env_bool("enabled") == EnvBool::Invalid);
static_assert(parse_env_bool("0 1") == EnvBool::Invalid);
static_assert(parse_env_bool("-1") == EnvBool::Invalid);
