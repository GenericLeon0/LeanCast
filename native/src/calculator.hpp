#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace leancast::calculator {

struct Result {
  std::wstring expression;
  std::wstring display;
  double value = 0.0;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring FormatNumber(double value) {
  if (!std::isfinite(value)) return L"";
  if (std::fabs(value) < 0.0000000001) value = 0.0;

  std::wostringstream stream;
  const double magnitude = std::fabs(value);
  if (magnitude >= 10000000000.0 || (magnitude > 0.0 && magnitude < 0.000001)) {
    stream << std::setprecision(10) << value;
    return stream.str();
  }

  stream << std::fixed << std::setprecision(10) << value;
  std::wstring out = stream.str();
  while (!out.empty() && out.back() == L'0') out.pop_back();
  if (!out.empty() && out.back() == L'.') out.pop_back();
  if (out == L"-0") out = L"0";
  return out;
}

class Parser {
 public:
  explicit Parser(std::wstring text) : text_(std::move(text)) {}

  bool Parse(double& value) {
    SkipSpaces();
    if (!ParseExpression(value)) return false;
    SkipSpaces();
    return pos_ == text_.size() && sawDigit_ && sawCalculationSyntax_ && std::isfinite(value);
  }

 private:
  bool ParseExpression(double& value) {
    if (!ParseTerm(value)) return false;
    while (true) {
      SkipSpaces();
      if (Match(L'+')) {
        sawCalculationSyntax_ = true;
        double rhs = 0.0;
        if (!ParseTerm(rhs)) return false;
        value += rhs;
      } else if (Match(L'-')) {
        sawCalculationSyntax_ = true;
        double rhs = 0.0;
        if (!ParseTerm(rhs)) return false;
        value -= rhs;
      } else {
        return true;
      }
    }
  }

  bool ParseTerm(double& value) {
    if (!ParseFactor(value)) return false;
    while (true) {
      SkipSpaces();
      if (Match(L'*') || Match(L'x') || Match(L'X')) {
        sawCalculationSyntax_ = true;
        double rhs = 0.0;
        if (!ParseFactor(rhs)) return false;
        value *= rhs;
      } else if (Match(L'/') || Match(L':')) {
        sawCalculationSyntax_ = true;
        double rhs = 0.0;
        if (!ParseFactor(rhs) || std::fabs(rhs) < 0.0000000001) return false;
        value /= rhs;
      } else {
        return true;
      }
    }
  }

  bool ParseFactor(double& value) {
    SkipSpaces();
    if (Match(L'+')) return ParseFactor(value);
    if (Match(L'-')) {
      if (!ParseFactor(value)) return false;
      value = -value;
      return true;
    }

    if (Match(L'(')) {
      sawCalculationSyntax_ = true;
      if (!ParseExpression(value)) return false;
      SkipSpaces();
      if (!Match(L')')) return false;
    } else if (!ParseNumber(value)) {
      return false;
    }

    while (true) {
      SkipSpaces();
      if (!Match(L'%')) return true;
      sawCalculationSyntax_ = true;
      value /= 100.0;
    }
  }

  bool ParseNumber(double& value) {
    SkipSpaces();
    const size_t start = pos_;
    bool hasDigit = false;
    bool hasSeparator = false;
    while (pos_ < text_.size()) {
      const wchar_t ch = text_[pos_];
      if (std::iswdigit(ch)) {
        hasDigit = true;
        ++pos_;
      } else if ((ch == L'.' || ch == L',') && !hasSeparator) {
        hasSeparator = true;
        ++pos_;
      } else {
        break;
      }
    }

    if (!hasDigit) return false;
    sawDigit_ = true;
    std::wstring number = text_.substr(start, pos_ - start);
    std::replace(number.begin(), number.end(), L',', L'.');
    wchar_t* end = nullptr;
    value = std::wcstod(number.c_str(), &end);
    return end && *end == L'\0' && std::isfinite(value);
  }

  void SkipSpaces() {
    while (pos_ < text_.size() && std::iswspace(text_[pos_])) ++pos_;
  }

  bool Match(wchar_t expected) {
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  std::wstring text_;
  size_t pos_ = 0;
  bool sawDigit_ = false;
  bool sawCalculationSyntax_ = false;
};

inline std::optional<Result> TryEvaluate(std::wstring input) {
  input = Trim(std::move(input));
  if (input.empty()) return std::nullopt;
  if (input.front() == L'=') input = Trim(input.substr(1));
  if (input.empty()) return std::nullopt;

  double value = 0.0;
  Parser parser(input);
  if (!parser.Parse(value)) return std::nullopt;

  std::wstring display = FormatNumber(value);
  if (display.empty()) return std::nullopt;
  return Result{input, display, value};
}

}  // namespace leancast::calculator
