#include "core/MvmeInitScript.h"

#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace fidget {
namespace {

using Json = MvmeWorkspace::JsonDocument;

struct ValueResult
{
    bool success = false;
    std::uint32_t value = 0U;
    MvmeInitScriptUnresolvedReason reason =
        MvmeInitScriptUnresolvedReason::UnsupportedExpression;
};

struct FloatingValueResult
{
    bool success = false;
    long double value = 0.0L;
    MvmeInitScriptUnresolvedReason reason =
        MvmeInitScriptUnresolvedReason::UnsupportedExpression;
};

struct VariableBinding
{
    bool resolved = false;
    std::string value;
};

using VariableTable = std::map<std::string, VariableBinding>;

std::string_view Trim(std::string_view text)
{
    while (!text.empty()
           && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1U);
    }
    while (!text.empty()
           && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1U);
    }
    return text;
}

std::string Lower(std::string_view text)
{
    std::string result(text);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

bool IsIdentifier(const std::string_view text)
{
    if (text.empty())
        return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (std::isalpha(first) == 0 && first != '_')
        return false;
    return std::all_of(
        text.begin() + 1, text.end(), [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '_';
        });
}

class ArithmeticParser
{
public:
    explicit ArithmeticParser(const std::string_view text)
        : text_(text)
    {
    }

    std::optional<long double> Parse()
    {
        const auto value = ParseExpression();
        SkipSpaces();
        if (!value.has_value() || position_ != text_.size()
            || !std::isfinite(*value))
        {
            return std::nullopt;
        }
        return value;
    }

private:
    std::optional<long double> ParseExpression()
    {
        auto left = ParseTerm();
        while (left.has_value())
        {
            SkipSpaces();
            if (!Consume('+') && !Consume('-'))
                break;
            const auto operation = text_[position_ - 1U];
            auto right = ParseTerm();
            if (!right.has_value())
                return std::nullopt;
            *left = operation == '+' ? *left + *right : *left - *right;
        }
        return left;
    }

    std::optional<long double> ParseTerm()
    {
        auto left = ParseFactor();
        while (left.has_value())
        {
            SkipSpaces();
            if (!Consume('*') && !Consume('/'))
                break;
            const auto operation = text_[position_ - 1U];
            auto right = ParseFactor();
            if (!right.has_value() || (operation == '/' && *right == 0.0L))
                return std::nullopt;
            *left = operation == '*' ? *left * *right : *left / *right;
        }
        return left;
    }

    std::optional<long double> ParseFactor()
    {
        SkipSpaces();
        if (Consume('+'))
            return ParseFactor();
        if (Consume('-'))
        {
            auto value = ParseFactor();
            if (value.has_value())
                *value = -*value;
            return value;
        }
        if (Consume('('))
        {
            auto value = ParseExpression();
            SkipSpaces();
            if (!value.has_value() || !Consume(')'))
                return std::nullopt;
            return value;
        }
        return ParseNumber();
    }

    std::optional<long double> ParseNumber()
    {
        SkipSpaces();
        if (position_ >= text_.size())
            return std::nullopt;

        if (position_ + 2U <= text_.size() && text_[position_] == '0'
            && (text_[position_ + 1U] == 'x'
                || text_[position_ + 1U] == 'X'))
        {
            position_ += 2U;
            return ParseIntegerDigits(16U, false);
        }
        if (position_ + 2U <= text_.size() && text_[position_] == '0'
            && (text_[position_ + 1U] == 'b'
                || text_[position_ + 1U] == 'B'))
        {
            position_ += 2U;
            return ParseIntegerDigits(2U, true);
        }

        const auto start = position_;
        bool digitSeen = false;
        bool pointSeen = false;
        long double integer = 0.0L;
        long double fraction = 0.0L;
        long double divisor = 1.0L;
        while (position_ < text_.size())
        {
            const auto character = text_[position_];
            if (character >= '0' && character <= '9')
            {
                digitSeen = true;
                const auto digit = static_cast<unsigned>(character - '0');
                if (!pointSeen)
                    integer = integer * 10.0L + digit;
                else
                {
                    divisor *= 10.0L;
                    fraction += digit / divisor;
                }
                ++position_;
                continue;
            }
            if (character == '.' && !pointSeen)
            {
                pointSeen = true;
                ++position_;
                continue;
            }
            break;
        }
        if (!digitSeen || position_ == start)
            return std::nullopt;
        return integer + fraction;
    }

    std::optional<long double> ParseIntegerDigits(
        const unsigned base,
        const bool allowSeparators)
    {
        bool digitSeen = false;
        long double value = 0.0L;
        while (position_ < text_.size())
        {
            const auto character = text_[position_];
            if (allowSeparators && character == '\'')
            {
                ++position_;
                continue;
            }

            unsigned digit = base;
            if (character >= '0' && character <= '9')
                digit = static_cast<unsigned>(character - '0');
            else if (character >= 'a' && character <= 'f')
                digit = static_cast<unsigned>(character - 'a') + 10U;
            else if (character >= 'A' && character <= 'F')
                digit = static_cast<unsigned>(character - 'A') + 10U;
            if (digit >= base)
                break;

            digitSeen = true;
            value = value * base + digit;
            ++position_;
        }
        if (!digitSeen)
            return std::nullopt;
        return value;
    }

    void SkipSpaces()
    {
        while (position_ < text_.size()
               && std::isspace(
                   static_cast<unsigned char>(text_[position_])) != 0)
        {
            ++position_;
        }
    }

    bool Consume(const char expected)
    {
        if (position_ >= text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    std::string_view text_;
    std::size_t position_ = 0U;
};

std::string FormatNumber(const long double value)
{
    if (value >= 0.0L
        && value <= static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max())
        && std::floor(value) == value)
    {
        return std::to_string(static_cast<std::uint64_t>(value));
    }

    std::ostringstream output;
    output << std::setprecision(
                  std::numeric_limits<long double>::max_digits10)
           << value;
    return output.str();
}

class ValueResolver
{
public:
    explicit ValueResolver(std::vector<VariableTable> tables)
        : tables_(std::move(tables))
    {
        if (tables_.empty())
            tables_.emplace_back();
    }

    ValueResult Resolve(const std::string_view source) const
    {
        const auto number = ResolveFloating(source);
        if (!number.success)
        {
            ValueResult result;
            result.reason = number.reason;
            return result;
        }

        const auto rounded = std::round(number.value);
        if (!std::isfinite(rounded) || rounded < 0.0L
            || rounded > static_cast<long double>(
                std::numeric_limits<std::uint32_t>::max()))
        {
            ValueResult result;
            result.reason = MvmeInitScriptUnresolvedReason::InvalidValue;
            return result;
        }

        ValueResult result;
        result.success = true;
        result.value = static_cast<std::uint32_t>(rounded);
        return result;
    }

    void SetLiteral(
        const std::string_view name,
        const std::string_view value)
    {
        auto& binding = tables_.front()[std::string(name)];
        binding.resolved = true;
        binding.value = std::string(value);
    }

    void Invalidate(const std::string_view name)
    {
        tables_.front()[std::string(name)] = {};
    }

    struct TextResult
    {
        bool success = false;
        std::string text;
        MvmeInitScriptUnresolvedReason reason =
            MvmeInitScriptUnresolvedReason::UnsupportedExpression;
    };

    TextResult ExpandAndEvaluateOnce(const std::string_view source) const
    {
        const auto expanded = ExpandVariablesOnce(source);
        if (!expanded.success)
            return expanded;
        return EvaluateExpressionsOnce(expanded.text);
    }

private:
    FloatingValueResult ResolveFloating(const std::string_view source) const
    {
        const auto text = Trim(source);
        if (text.find("${") != std::string_view::npos
            || text.find("$(") != std::string_view::npos)
        {
            FloatingValueResult result;
            result.reason =
                MvmeInitScriptUnresolvedReason::ExpansionLimitReached;
            return result;
        }
        if (text.empty())
            return {};

        std::optional<long double> number;
        if (text.size() > 2U && text[0U] == '0'
            && (text[1U] == 'x' || text[1U] == 'X'))
        {
            std::uint64_t value = 0U;
            const auto converted = std::from_chars(
                text.data() + 2U,
                text.data() + text.size(),
                value,
                16);
            if (converted.ec == std::errc{}
                && converted.ptr == text.data() + text.size())
            {
                number = static_cast<long double>(value);
            }
        }
        else if (text.size() > 2U && text[0U] == '0'
                 && (text[1U] == 'b' || text[1U] == 'B'))
        {
            std::uint64_t value = 0U;
            bool digitSeen = false;
            bool valid = true;
            for (const auto character : text.substr(2U))
            {
                if (character == '\'')
                    continue;
                if (character != '0' && character != '1')
                {
                    valid = false;
                    break;
                }
                digitSeen = true;
                if (value > (std::numeric_limits<std::uint64_t>::max() >> 1U))
                {
                    valid = false;
                    break;
                }
                value = (value << 1U)
                    | static_cast<std::uint64_t>(character - '0');
            }
            if (valid && digitSeen)
                number = static_cast<long double>(value);
        }
        else if (text.find('.') != std::string_view::npos)
        {
            std::string valueText(text);
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtof(valueText.c_str(), &end);
            if (errno != ERANGE && end == valueText.c_str() + valueText.size()
                && std::isfinite(value))
            {
                number = value;
            }
        }
        else if (std::all_of(
                     text.begin(), text.end(), [](const unsigned char value) {
                         return std::isdigit(value) != 0;
                     }))
        {
            std::uint64_t value = 0U;
            const auto converted = std::from_chars(
                text.data(), text.data() + text.size(), value, 10);
            if (converted.ec == std::errc{}
                && converted.ptr == text.data() + text.size())
            {
                number = static_cast<long double>(value);
            }
        }

        if (!number.has_value())
        {
            FloatingValueResult result;
            const auto firstOperator = text.find_first_of("+*/()");
            const auto nonLeadingMinus = text.find('-', 1U);
            result.reason = firstOperator != std::string_view::npos
                    || nonLeadingMinus != std::string_view::npos
                ? MvmeInitScriptUnresolvedReason::
                    ArithmeticRequiresExpression
                : MvmeInitScriptUnresolvedReason::UnsupportedExpression;
            return result;
        }
        if (!std::isfinite(*number))
        {
            FloatingValueResult result;
            result.reason = MvmeInitScriptUnresolvedReason::InvalidValue;
            return result;
        }

        FloatingValueResult result;
        result.success = true;
        result.value = *number;
        return result;
    }

    TextResult ExpandVariablesOnce(const std::string_view source) const
    {
        std::string expanded;
        std::size_t position = 0U;
        while (position < source.size())
        {
            const auto variableStart = source.find("${", position);
            if (variableStart == std::string_view::npos)
            {
                expanded.append(source.substr(position));
                break;
            }
            expanded.append(source.substr(position, variableStart - position));
            const auto variableEnd = source.find('}', variableStart + 2U);
            if (variableEnd == std::string_view::npos)
            {
                TextResult result;
                result.reason =
                    MvmeInitScriptUnresolvedReason::UndefinedVariable;
                return result;
            }
            const auto name = std::string(
                source.substr(
                    variableStart + 2U,
                    variableEnd - variableStart - 2U));
            if (!IsIdentifier(name))
            {
                TextResult result;
                result.reason =
                    MvmeInitScriptUnresolvedReason::UndefinedVariable;
                return result;
            }

            const auto binding = Lookup(name);
            if (!binding.has_value() || !binding->resolved)
            {
                TextResult result;
                result.reason =
                    MvmeInitScriptUnresolvedReason::UndefinedVariable;
                return result;
            }

            expanded.append(binding->value);
            position = variableEnd + 1U;
        }
        TextResult result;
        result.success = true;
        result.text = std::move(expanded);
        return result;
    }

    static TextResult EvaluateExpressionsOnce(const std::string_view source)
    {
        TextResult result;
        std::size_t position = 0U;
        while (position < source.size())
        {
            const auto expressionStart = source.find("$(", position);
            if (expressionStart == std::string_view::npos)
            {
                result.text.append(source.substr(position));
                break;
            }
            result.text.append(
                source.substr(position, expressionStart - position));

            std::size_t expressionEnd = expressionStart + 2U;
            unsigned depth = 1U;
            for (; expressionEnd < source.size(); ++expressionEnd)
            {
                if (source[expressionEnd] == '(')
                    ++depth;
                else if (source[expressionEnd] == ')' && --depth == 0U)
                    break;
            }
            if (depth != 0U)
            {
                result.reason =
                    MvmeInitScriptUnresolvedReason::UnsupportedExpression;
                return result;
            }

            const auto expression = source.substr(
                expressionStart + 2U,
                expressionEnd - expressionStart - 2U);
            const auto value = ArithmeticParser(expression).Parse();
            if (!value.has_value())
            {
                result.reason =
                    MvmeInitScriptUnresolvedReason::UnsupportedExpression;
                return result;
            }
            result.text += FormatNumber(*value);
            position = expressionEnd + 1U;
        }
        result.success = true;
        return result;
    }

    std::optional<VariableBinding> Lookup(const std::string& name) const
    {
        for (const auto& table : tables_)
        {
            const auto found = table.find(name);
            if (found != table.end())
                return found->second;
        }
        return std::nullopt;
    }

    std::vector<VariableTable> tables_;
};

struct VariableTableResult
{
    bool success = false;
    VariableTable table;
    std::string message;
};

VariableTableResult JsonVariableTable(
    const Json& owner,
    const char* fieldName)
{
    VariableTableResult result;
    const auto found = owner.find(fieldName);
    if (found == owner.end())
    {
        result.success = true;
        return result;
    }
    if (!found->is_object())
    {
        result.message = "A variable_table field is not an object.";
        return result;
    }

    for (const auto& item : found->items())
    {
        if (!IsIdentifier(item.key()) || !item.value().is_object())
        {
            result.message =
                "A variable_table entry is not a named variable object.";
            return result;
        }
        const auto value = item.value().find("value");
        if (value == item.value().end()
            || (!value->is_string() && !value->is_number()))
        {
            result.message =
                "A variable_table entry has no string or numeric value.";
            return result;
        }
        const auto unit = item.value().find("unit");
        if (unit != item.value().end() && !unit->is_string())
        {
            result.message =
                "A variable_table entry has a non-string unit.";
            return result;
        }

        VariableBinding binding;
        binding.resolved = true;
        binding.value = value->is_string()
            ? value->get<std::string>()
            : value->dump();
        result.table.emplace(item.key(), std::move(binding));
    }
    result.success = true;
    return result;
}

struct CommentStripResult
{
    std::string text;
    bool unterminatedBlock = false;
};

CommentStripResult StripComments(const std::string_view script)
{
    CommentStripResult result;
    result.text.reserve(script.size());
    bool inBlock = false;
    bool inLine = false;
    for (std::size_t index = 0U; index < script.size(); ++index)
    {
        const auto character = script[index];
        const auto next = index + 1U < script.size() ? script[index + 1U] : 0;
        if (inLine)
        {
            if (character == '\n')
            {
                inLine = false;
                result.text.push_back('\n');
            }
            else
                result.text.push_back(' ');
            continue;
        }
        if (inBlock)
        {
            if (character == '*' && next == '/')
            {
                result.text.append("  ");
                ++index;
                inBlock = false;
            }
            else if (character == '\n')
                result.text.push_back('\n');
            else
                result.text.push_back(' ');
            continue;
        }
        if (character == '#')
        {
            inLine = true;
            result.text.push_back(' ');
        }
        else if (character == '/' && next == '*')
        {
            inBlock = true;
            result.text.append("  ");
            ++index;
        }
        else
            result.text.push_back(character);
    }
    result.unterminatedBlock = inBlock;
    return result;
}

struct AtomicPartsResult
{
    bool success = false;
    std::vector<std::string> parts;
    MvmeInitScriptUnresolvedReason reason =
        MvmeInitScriptUnresolvedReason::MalformedScript;
};

// Mirrors MVME's split_into_atomic_parts(): quoted strings, ${...} variable
// references, and $(...) expressions each remain atomic and may be
// concatenated without whitespace.
AtomicPartsResult SplitAtomicParts(const std::string_view line)
{
    AtomicPartsResult result;
    std::string part;
    const auto flush = [&]() {
        if (!part.empty())
        {
            result.parts.push_back(std::move(part));
            part.clear();
        }
    };

    for (std::size_t index = 0U; index < line.size();)
    {
        const auto character = line[index];
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            flush();
            ++index;
            continue;
        }
        if (character == '"')
        {
            ++index;
            bool closed = false;
            while (index < line.size())
            {
                if (line[index] == '\\' && index + 1U < line.size())
                {
                    part.push_back(line[index + 1U]);
                    index += 2U;
                    continue;
                }
                if (line[index] == '"')
                {
                    ++index;
                    closed = true;
                    break;
                }
                part.push_back(line[index++]);
            }
            if (!closed)
                return result;
            continue;
        }
        if (character == '$' && index + 1U < line.size()
            && (line[index + 1U] == '{' || line[index + 1U] == '('))
        {
            const auto opening = line[index + 1U];
            const auto closing = opening == '{' ? '}' : ')';
            const auto start = index;
            index += 2U;
            unsigned depth = 1U;
            while (index < line.size() && depth != 0U)
            {
                if (opening == '(' && line[index] == opening)
                    ++depth;
                else if (line[index] == closing)
                    --depth;
                ++index;
            }
            if (depth != 0U)
            {
                result.reason = opening == '{'
                    ? MvmeInitScriptUnresolvedReason::UndefinedVariable
                    : MvmeInitScriptUnresolvedReason::UnsupportedExpression;
                return result;
            }
            part.append(line.substr(start, index - start));
            continue;
        }
        part.push_back(character);
        ++index;
    }
    flush();
    result.success = true;
    return result;
}

struct PreparedLine
{
    bool success = false;
    std::vector<std::string> parts;
    std::vector<std::string> rawParts;
    MvmeInitScriptUnresolvedReason reason =
        MvmeInitScriptUnresolvedReason::MalformedScript;
};

PreparedLine PrepareLine(
    const std::string_view line,
    const ValueResolver& resolver)
{
    PreparedLine result;
    auto initial = SplitAtomicParts(line);
    result.rawParts = initial.parts;
    if (!initial.success || initial.parts.empty())
    {
        result.reason = initial.reason;
        return result;
    }

    auto expand = [&](std::vector<std::string>& parts) {
        for (auto& part : parts)
        {
            const auto expanded = resolver.ExpandAndEvaluateOnce(part);
            if (!expanded.success)
            {
                result.reason = expanded.reason;
                return false;
            }
            part = expanded.text;
        }
        return true;
    };

    if (!expand(initial.parts) || initial.parts.empty())
        return result;
    initial.parts.front() = Lower(initial.parts.front());

    // MVME src/vme_script.cc performs exactly one optional command-specific
    // reparse/expansion pass. Fixed-arity set and accumulator tests skip it.
    const auto& command = initial.parts.front();
    if (command != "set" && command != "accu_test"
        && command != "accu_test_warn")
    {
        std::vector<std::string> reparsed;
        for (const auto& part : initial.parts)
        {
            auto split = SplitAtomicParts(part);
            if (!split.success)
            {
                result.reason = split.reason;
                return result;
            }
            reparsed.insert(
                reparsed.end(),
                std::make_move_iterator(split.parts.begin()),
                std::make_move_iterator(split.parts.end()));
        }
        if (reparsed.empty() || !expand(reparsed))
            return result;
        reparsed.front() = Lower(reparsed.front());
        initial.parts = std::move(reparsed);
    }

    if (std::any_of(
            initial.parts.begin(), initial.parts.end(),
            [](const std::string& part) {
                return part.find("${") != std::string::npos
                    || part.find("$(") != std::string::npos;
            }))
    {
        result.reason = MvmeInitScriptUnresolvedReason::ExpansionLimitReached;
        return result;
    }

    result.success = true;
    result.parts = std::move(initial.parts);
    return result;
}

class Evaluator
{
public:
    Evaluator(
        const MvmeWorkspace& workspace,
        const MvmeWorkspaceTarget& target)
        : workspace_(workspace)
        , target_(target)
    {
    }

    MvmeInitScriptEvaluation Run()
    {
        result_.state = MvmeInitScriptEvaluationState::Complete;
        const auto located = workspace_.FindEnabledMdpp32ScpTarget(
            target_.address);
        if (located.status != MvmeWorkspaceTargetStatus::Found
            || !located.target.has_value()
            || located.target->eventIndex != target_.eventIndex
            || located.target->moduleIndex != target_.moduleIndex)
        {
            return Malformed(
                "The supplied target is not the unique enabled mdpp32_scp "
                "module at its normalized address.");
        }

        try
        {
            const auto& daq = workspace_.Json().at("DAQConfig");
            const auto& events = daq.at("events");
            if (!events.is_array() || target_.eventIndex >= events.size())
                return Malformed("The target event index is not valid.");
            const auto& event = events[target_.eventIndex];
            const auto& modules = event.at("modules");
            if (!modules.is_array() || target_.moduleIndex >= modules.size())
                return Malformed("The target module index is not valid.");
            const auto& module = modules[target_.moduleIndex];
            const auto scripts = module.find("initScripts");
            if (scripts == module.end() || !scripts->is_array())
                return Malformed("The target initScripts field is not an array.");

            for (std::size_t scriptIndex = 0U;
                 scriptIndex < scripts->size();
                 ++scriptIndex)
            {
                const auto& script = (*scripts)[scriptIndex];
                if (!script.is_object())
                {
                    AddIssue(
                        {scriptIndex, 0U},
                        MvmeInitScriptUnresolvedImpact::Frontend,
                        MvmeInitScriptUnresolvedReason::MalformedScript,
                        "An init script entry is not an object.");
                    selector_.reset();
                    continue;
                }

                const auto enabled = script.find("enabled");
                if (enabled != script.end() && !enabled->is_boolean())
                {
                    AddIssue(
                        {scriptIndex, 0U},
                        MvmeInitScriptUnresolvedImpact::Frontend,
                        MvmeInitScriptUnresolvedReason::MalformedScript,
                        "An init script enabled field is not Boolean.");
                    selector_.reset();
                    continue;
                }
                if (enabled != script.end() && !enabled->get<bool>())
                    continue;

                ++result_.enabledScriptCount;
                const auto text = script.find("vme_script");
                if (text == script.end() || !text->is_string())
                {
                    AddIssue(
                        {scriptIndex, 0U},
                        MvmeInitScriptUnresolvedImpact::Frontend,
                        MvmeInitScriptUnresolvedReason::MalformedScript,
                        "An enabled init script has no text payload.");
                    selector_.reset();
                    continue;
                }

                std::vector<VariableTable> tables(1U);
                bool tablesValid = true;
                const auto addTable = [&](const Json& owner) {
                    const auto parsed = JsonVariableTable(
                        owner, "variable_table");
                    if (!parsed.success)
                    {
                        AddIssue(
                            {scriptIndex, 0U},
                            MvmeInitScriptUnresolvedImpact::Frontend,
                            MvmeInitScriptUnresolvedReason::
                                MalformedVariableTable,
                            parsed.message);
                        tablesValid = false;
                        return;
                    }
                    tables.push_back(parsed.table);
                };
                addTable(script);
                addTable(module);
                addTable(event);
                addTable(daq);
                if (!tablesValid)
                {
                    selector_.reset();
                    continue;
                }
                EvaluateScript(
                    scriptIndex,
                    text->get_ref<const std::string&>(),
                    ValueResolver(std::move(tables)));
            }
        }
        catch (const nlohmann::json::exception&)
        {
            return Malformed(
                "The target workspace structure is incomplete or malformed.");
        }

        for (const auto& item : finalValues_)
            result_.finalFrontendValues.push_back(item.second);
        Finalize();
        return result_;
    }

private:
    MvmeInitScriptEvaluation Malformed(std::string message)
    {
        AddIssue(
            {0U, 0U},
            MvmeInitScriptUnresolvedImpact::Frontend,
            MvmeInitScriptUnresolvedReason::MalformedScript,
            std::move(message));
        Finalize();
        return result_;
    }

    void EvaluateScript(
        const std::size_t scriptIndex,
        const std::string_view source,
        ValueResolver resolver)
    {
        const auto stripped = StripComments(source);
        std::size_t lineNumber = 1U;
        std::size_t lineStart = 0U;
        while (lineStart <= stripped.text.size())
        {
            const auto lineEnd = stripped.text.find('\n', lineStart);
            const auto length = lineEnd == std::string::npos
                ? stripped.text.size() - lineStart
                : lineEnd - lineStart;
            const auto line = Trim(
                std::string_view(stripped.text).substr(lineStart, length));
            if (!line.empty())
                EvaluateLine({scriptIndex, lineNumber}, line, resolver);
            if (lineEnd == std::string::npos)
                break;
            lineStart = lineEnd + 1U;
            ++lineNumber;
        }

        if (stripped.unterminatedBlock)
        {
            AddIssue(
                {scriptIndex, lineNumber},
                MvmeInitScriptUnresolvedImpact::Frontend,
                MvmeInitScriptUnresolvedReason::MalformedScript,
                "An init script contains an unterminated block comment.");
            selector_.reset();
        }
    }

    void EvaluateLine(
        const MvmeInitScriptLocation location,
        const std::string_view line,
        ValueResolver& resolver)
    {
        const auto prepared = PrepareLine(line, resolver);
        if (!prepared.success)
        {
            ReportPreparationFailure(location, prepared, resolver);
            return;
        }
        const auto& parts = prepared.parts;
        const auto& command = parts.front();
        if (statementsConditional_)
        {
            EvaluateConditionalLine(location, parts, resolver);
            return;
        }
        if (command == "accu_test")
        {
            result_.conditionalAccuTestLocation = location;
            conditionalSelector_ = selector_;
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::NonFrontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "An accu_test depends on a live accumulator; whether later "
                "statements execute cannot be proven.");
            statementsConditional_ = true;
            return;
        }
        if (command == "set")
        {
            EvaluateSet(location, parts, resolver);
            return;
        }
        if (command == "write" || command == "writeabs")
        {
            EvaluateLongWrite(
                location, command == "writeabs", parts, resolver);
            return;
        }
        if (IsKnownNonFrontendCommand(command))
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::NonFrontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "A non-frontend init-script statement was not interpreted.");
            return;
        }

        const auto address = resolver.Resolve(parts.front());
        if (!address.success)
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                address.reason,
                "A possible write address could not be resolved, so frontend "
                "impact cannot be excluded.");
            selector_.reset();
            return;
        }
        if (parts.size() != 2U)
        {
            AddIssue(
                location,
                IsFrontendAddress(address.value)
                    ? MvmeInitScriptUnresolvedImpact::Frontend
                    : MvmeInitScriptUnresolvedImpact::NonFrontend,
                parts.size() < 2U
                    ? MvmeInitScriptUnresolvedReason::UnsupportedExpression
                    : MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "A shorthand write does not have exactly one value.");
            if (address.value == Fw2051ScpSelectorRegister)
                selector_.reset();
            return;
        }
        EvaluateRelativeWrite(
            location, address.value, parts[1U], resolver, true);
    }

    void ReportPreparationFailure(
        const MvmeInitScriptLocation location,
        const PreparedLine& prepared,
        ValueResolver& resolver)
    {
        const auto command = prepared.rawParts.empty()
            ? std::string{}
            : Lower(prepared.rawParts.front());
        auto nonFrontend = command == "set"
            || command == "accu_test"
            || IsKnownNonFrontendCommand(command);
        if (!nonFrontend && !prepared.rawParts.empty())
        {
            if ((command == "write" || command == "writeabs")
                && prepared.rawParts.size() > 3U)
            {
                const auto address = resolver.Resolve(prepared.rawParts[3U]);
                if (address.success)
                {
                    const auto relative = command == "writeabs"
                        ? RelativeAbsoluteAddress(address.value)
                        : std::optional<std::uint32_t>(address.value);
                    nonFrontend = relative.has_value()
                        && !IsFrontendAddress(*relative);
                }
            }
            else
            {
                const auto address = resolver.Resolve(
                    prepared.rawParts.front());
                nonFrontend = address.success
                    && !IsFrontendAddress(address.value);
            }
        }
        if (command == "set" && prepared.rawParts.size() > 1U
            && IsIdentifier(prepared.rawParts[1U]))
        {
            resolver.Invalidate(prepared.rawParts[1U]);
        }
        AddIssue(
            location,
            nonFrontend
                ? MvmeInitScriptUnresolvedImpact::NonFrontend
                : MvmeInitScriptUnresolvedImpact::Frontend,
            prepared.reason,
            nonFrontend
                ? "A non-frontend statement could not be expanded using "
                  "MVME's bounded parsing model."
                : "A possible frontend write could not be expanded using "
                  "MVME's bounded parsing model.");
        if (!nonFrontend)
            selector_.reset();
    }

    void EvaluateConditionalLine(
        const MvmeInitScriptLocation location,
        const std::vector<std::string>& parts,
        ValueResolver& resolver)
    {
        const auto& command = parts.front();
        auto impact = MvmeInitScriptUnresolvedImpact::Frontend;
        std::optional<std::uint32_t> frontendAddress;
        std::string_view valueText;
        if (command == "set")
        {
            if (parts.size() > 1U && IsIdentifier(parts[1U]))
                resolver.Invalidate(parts[1U]);
            impact = MvmeInitScriptUnresolvedImpact::NonFrontend;
        }
        else if (IsKnownNonFrontendCommand(command)
                 || command == "accu_test")
        {
            impact = MvmeInitScriptUnresolvedImpact::NonFrontend;
        }
        else if (command == "write" || command == "writeabs")
        {
            if (parts.size() >= 4U)
            {
                const auto address = resolver.Resolve(parts[3U]);
                if (address.success)
                {
                    const auto relativeAddress = command == "writeabs"
                        ? RelativeAbsoluteAddress(address.value)
                        : std::optional<std::uint32_t>(address.value);
                    if (relativeAddress.has_value()
                        && !IsFrontendAddress(*relativeAddress))
                    {
                        impact = MvmeInitScriptUnresolvedImpact::NonFrontend;
                    }
                    else
                    {
                        frontendAddress = relativeAddress;
                        if (parts.size() > 4U)
                            valueText = parts[4U];
                    }
                }
            }
        }
        else
        {
            const auto address = resolver.Resolve(parts.front());
            if (address.success && !IsFrontendAddress(address.value))
                impact = MvmeInitScriptUnresolvedImpact::NonFrontend;
            else if (address.success)
            {
                frontendAddress = address.value;
                if (parts.size() > 1U)
                    valueText = parts[1U];
            }
        }

        if (impact == MvmeInitScriptUnresolvedImpact::Frontend)
        {
            conditionalFrontendWrite_ = true;
            TaintConditionalFrontendValue(
                frontendAddress, valueText, resolver);
        }
        AddIssue(
            location,
            impact,
            MvmeInitScriptUnresolvedReason::ConditionalAfterAccuTest,
            impact == MvmeInitScriptUnresolvedImpact::Frontend
                ? "A possible frontend write is conditional after an "
                  "unprovable accu_test and was not extracted."
                : "A non-frontend statement is conditional after an "
                  "unprovable accu_test and was not interpreted.");
    }

    void TaintConditionalFrontendValue(
        const std::optional<std::uint32_t> address,
        const std::string_view valueText,
        const ValueResolver& resolver)
    {
        if (!address.has_value()
            || *address > std::numeric_limits<std::uint16_t>::max())
        {
            finalValues_.clear();
            conditionalSelector_.reset();
            return;
        }

        const auto registerOffset = static_cast<std::uint16_t>(*address);
        if (registerOffset == Fw2051ScpSelectorRegister)
        {
            const auto value = resolver.Resolve(valueText);
            if (!value.success
                || value.value > Fw2051ScpBroadcastSelectorValue)
            {
                conditionalSelector_.reset();
            }
            else
            {
                conditionalSelector_ =
                    static_cast<std::uint16_t>(value.value);
            }
            return;
        }

        if (FindFw2051ScpSetting(registerOffset) == nullptr)
        {
            finalValues_.clear();
            return;
        }
        if (!conditionalSelector_.has_value()
            || *conditionalSelector_ == Fw2051ScpBroadcastSelectorValue)
        {
            for (std::uint16_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
                finalValues_.erase({quad, registerOffset});
            return;
        }
        finalValues_.erase({*conditionalSelector_, registerOffset});
    }

    void EvaluateSet(
        const MvmeInitScriptLocation location,
        const std::vector<std::string>& parts,
        ValueResolver& resolver)
    {
        const auto nameValid = parts.size() > 1U
            && IsIdentifier(parts[1U]);
        if (parts.size() != 3U || !nameValid)
        {
            if (nameValid)
                resolver.Invalidate(parts[1U]);
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::NonFrontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "A set statement does not match MVME's exact three-part "
                "form.");
            return;
        }
        resolver.SetLiteral(parts[1U], parts[2U]);
    }

    void EvaluateLongWrite(
        const MvmeInitScriptLocation location,
        const bool absolute,
        const std::vector<std::string>& parts,
        ValueResolver& resolver)
    {
        if (parts.size() < 4U)
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "A write form has no provable target address.");
            selector_.reset();
            return;
        }
        const auto address = resolver.Resolve(parts[3U]);
        if (!address.success)
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                address.reason,
                "A write address could not be resolved, so frontend impact "
                "cannot be excluded.");
            selector_.reset();
            return;
        }

        const auto relativeAddress = absolute
            ? RelativeAbsoluteAddress(address.value)
            : std::optional<std::uint32_t>(address.value);
        const auto supportedForm = parts.size() == 5U
            && Lower(parts[1U]) == "a32" && Lower(parts[2U]) == "d16"
            && !absolute;
        if (!supportedForm)
        {
            const auto frontend = relativeAddress.has_value()
                && IsFrontendAddress(*relativeAddress);
            AddIssue(
                location,
                frontend
                    ? MvmeInitScriptUnresolvedImpact::Frontend
                    : MvmeInitScriptUnresolvedImpact::NonFrontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                frontend
                    ? "An unsupported write form could affect a target "
                      "frontend register."
                    : "An unsupported write form was outside the target "
                      "frontend subset.");
            if (!relativeAddress.has_value()
                || *relativeAddress == Fw2051ScpSelectorRegister)
            {
                selector_.reset();
            }
            return;
        }

        EvaluateRelativeWrite(
            location,
            *relativeAddress,
            parts[4U],
            resolver,
            true);
    }

    void EvaluateRelativeWrite(
        const MvmeInitScriptLocation location,
        const std::uint32_t address,
        const std::string_view valueText,
        ValueResolver& resolver,
        const bool supportedForm)
    {
        if (!supportedForm)
            return;
        if (!IsFrontendAddress(address))
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::NonFrontend,
                MvmeInitScriptUnresolvedReason::UnsupportedStatement,
                "A target-module write outside the frontend subset was not "
                "interpreted.");
            return;
        }

        const auto value = resolver.Resolve(valueText);
        if (!value.success || value.value > std::numeric_limits<std::uint16_t>::max())
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                value.success
                    ? MvmeInitScriptUnresolvedReason::InvalidValue
                    : value.reason,
                address == Fw2051ScpSelectorRegister
                    ? "The frontend selector value could not be resolved."
                    : "A target frontend value could not be resolved.");
            if (address == Fw2051ScpSelectorRegister)
                selector_.reset();
            return;
        }

        const auto registerOffset = static_cast<std::uint16_t>(address);
        const auto registerValue = static_cast<std::uint16_t>(value.value);
        if (registerOffset == Fw2051ScpSelectorRegister)
        {
            SetSelector(location, registerValue);
            return;
        }

        if (!selector_.has_value())
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                MvmeInitScriptUnresolvedReason::SelectorUnresolved,
                "A banked frontend write cannot be attributed because the "
                "selector state is unresolved.");
            return;
        }

        const auto scope = *selector_ == Fw2051ScpBroadcastSelectorValue
            ? MvmeInitScriptSelectorScope::Broadcast
            : MvmeInitScriptSelectorScope::Quad;
        result_.frontendWrites.push_back({
            location,
            registerOffset,
            registerValue,
            *selector_,
            scope,
        });
        if (scope == MvmeInitScriptSelectorScope::Broadcast)
        {
            for (std::uint16_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
                SetFinalValue(location, quad, registerOffset, registerValue);
        }
        else
            SetFinalValue(
                location, *selector_, registerOffset, registerValue);
    }

    void SetSelector(
        const MvmeInitScriptLocation location,
        const std::uint16_t value)
    {
        if (value > Fw2051ScpBroadcastSelectorValue)
        {
            AddIssue(
                location,
                MvmeInitScriptUnresolvedImpact::Frontend,
                MvmeInitScriptUnresolvedReason::InvalidSelector,
                "The selector value is outside the FW2051 quad/broadcast "
                "model.");
            selector_.reset();
            return;
        }

        selector_ = value;
        result_.selectorAssignments.push_back({
            location,
            value,
            value == Fw2051ScpBroadcastSelectorValue
                ? MvmeInitScriptSelectorScope::Broadcast
                : MvmeInitScriptSelectorScope::Quad,
        });
    }

    void SetFinalValue(
        const MvmeInitScriptLocation location,
        const std::uint16_t quad,
        const std::uint16_t registerOffset,
        const std::uint16_t value)
    {
        finalValues_[{quad, registerOffset}] = {
            location,
            quad,
            registerOffset,
            value,
        };
    }

    std::optional<std::uint32_t> RelativeAbsoluteAddress(
        const std::uint32_t absoluteAddress) const
    {
        const auto base = target_.address.FullA32Value();
        if (absoluteAddress < base)
            return std::nullopt;
        return absoluteAddress - base;
    }

    static bool IsFrontendAddress(const std::uint32_t address)
    {
        if (address > std::numeric_limits<std::uint16_t>::max())
            return false;
        const auto offset = static_cast<std::uint16_t>(address);
        return offset == Fw2051ScpSelectorRegister
            || FindFw2051ScpSetting(offset) != nullptr;
    }

    static bool IsKnownNonFrontendCommand(const std::string_view command)
    {
        return command == "read" || command == "readabs"
            || command == "accu_mask_rotate"
            || command == "accu_test_warn" || command == "wait"
            || command == "print";
    }

    void AddIssue(
        const MvmeInitScriptLocation location,
        const MvmeInitScriptUnresolvedImpact impact,
        const MvmeInitScriptUnresolvedReason reason,
        std::string message)
    {
        result_.unresolvedStatements.push_back(
            {location, impact, reason, std::move(message)});
    }

    void Finalize()
    {
        const auto frontendFailure = std::any_of(
            result_.unresolvedStatements.begin(),
            result_.unresolvedStatements.end(),
            [](const MvmeInitScriptUnresolvedStatement& unresolved) {
                return unresolved.impact
                        == MvmeInitScriptUnresolvedImpact::Frontend
                    && unresolved.reason
                        != MvmeInitScriptUnresolvedReason::
                            ConditionalAfterAccuTest;
            });
        if (frontendFailure)
        {
            result_.state = MvmeInitScriptEvaluationState::Failed;
            result_.message =
                "Init-script evaluation failed because at least one "
                "frontend-affecting statement was unresolved.";
        }
        else if (conditionalFrontendWrite_)
        {
            result_.state = MvmeInitScriptEvaluationState::
                ConditionalAfterAccuTest;
            result_.message =
                "Later frontend writes were conditional after an "
                "unprovable accu_test and were not extracted.";
        }
        else if (!result_.unresolvedStatements.empty())
        {
            result_.state = MvmeInitScriptEvaluationState::
                CompleteWithUnresolvedNonFrontend;
            result_.message =
                "Frontend writes were resolved; non-frontend statements "
                "remain explicitly unresolved.";
        }
        else
        {
            result_.state = MvmeInitScriptEvaluationState::Complete;
            result_.message =
                "All enabled target init scripts were evaluated.";
        }
    }

    const MvmeWorkspace& workspace_;
    const MvmeWorkspaceTarget& target_;
    MvmeInitScriptEvaluation result_;
    std::optional<std::uint16_t> selector_;
    std::optional<std::uint16_t> conditionalSelector_;
    bool statementsConditional_ = false;
    bool conditionalFrontendWrite_ = false;
    std::map<
        std::pair<std::uint16_t, std::uint16_t>,
        MvmeInitScriptFrontendValue> finalValues_;
};

} // namespace

MvmeInitScriptEvaluation EvaluateMvmeTargetInitScripts(
    const MvmeWorkspace& workspace,
    const MvmeWorkspaceTarget& target)
{
    return Evaluator(workspace, target).Run();
}

} // namespace fidget
