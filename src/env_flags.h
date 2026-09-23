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

// Integer environment values, for test-only controls that have to be recorded
// in a result record.  A reference capture whose seed cannot be stated is a
// capture nobody can repeat, so a wrong value must stop the run rather than
// quietly fall back to the default and produce a file labelled with a seed it
// was not rendered at.
enum class EnvIntStatus {
    Absent,   // absent or empty: the caller keeps its own default
    Valid,    // a complete integer, in range
    Invalid,  // anything else: the caller fails the run
};

struct EnvInt {
    EnvIntStatus status;
    int value;

    constexpr bool operator==(const EnvInt& other) const
    {
        return status == other.status && value == other.value;
    }
};

// Strict on purpose: the whole trimmed string has to be one integer, so "7x",
// "0x10", "1 2", "7.0" and "" are refused rather than silently read as 7, 0, 1
// and 7.  Overflow is refused too, instead of wrapping into a seed nobody
// chose.
constexpr EnvInt parse_env_int(const char* value)
{
    if (value == nullptr) {
        return {EnvIntStatus::Absent, 0};
    }
    const std::string_view text = env_flags_detail::trim(value);
    if (text.empty()) {
        // `set VAR=` is how a batch file clears a variable; treating it as
        // absent keeps it saying the same thing as never setting it at all.
        return {EnvIntStatus::Absent, 0};
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index >= text.size()) {
        return {EnvIntStatus::Invalid, 0};
    }
    // Accumulated as long long so the range check is a comparison rather than
    // signed overflow, which would be undefined and is not constant-evaluable.
    long long accumulated = 0;
    for (; index < text.size(); ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') {
            return {EnvIntStatus::Invalid, 0};
        }
        accumulated = accumulated * 10 + (digit - '0');
        if (accumulated > 2147483648LL) {
            return {EnvIntStatus::Invalid, 0};
        }
    }
    const long long signed_value = negative ? -accumulated : accumulated;
    if (signed_value < -2147483648LL || signed_value > 2147483647LL) {
        return {EnvIntStatus::Invalid, 0};
    }
    return {EnvIntStatus::Valid, static_cast<int>(signed_value)};
}

static_assert(parse_env_int(nullptr).status == EnvIntStatus::Absent);
static_assert(parse_env_int("").status == EnvIntStatus::Absent);
static_assert(parse_env_int("   ").status == EnvIntStatus::Absent);
static_assert(parse_env_int("0") == EnvInt{EnvIntStatus::Valid, 0});
static_assert(parse_env_int("1337") == EnvInt{EnvIntStatus::Valid, 1337});
static_assert(parse_env_int(" 1337 ") == EnvInt{EnvIntStatus::Valid, 1337});
static_assert(parse_env_int("\t42\r\n") == EnvInt{EnvIntStatus::Valid, 42});
static_assert(parse_env_int("-7") == EnvInt{EnvIntStatus::Valid, -7});
static_assert(parse_env_int("+7") == EnvInt{EnvIntStatus::Valid, 7});
static_assert(parse_env_int("007") == EnvInt{EnvIntStatus::Valid, 7});
static_assert(parse_env_int("2147483647") == EnvInt{EnvIntStatus::Valid, 2147483647});
static_assert(parse_env_int("-2147483648") == EnvInt{EnvIntStatus::Valid, -2147483647 - 1});
static_assert(parse_env_int("2147483648").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("-2147483649").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("99999999999999999999").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("7x").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("x7").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("0x10").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("7.0").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("1 2").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("-").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("+").status == EnvIntStatus::Invalid);
static_assert(parse_env_int("--1").status == EnvIntStatus::Invalid);
