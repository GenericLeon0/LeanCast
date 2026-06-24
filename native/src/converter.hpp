#pragma once

// Pure unit and currency conversion engine, mirroring calculator.hpp so it can
// be unit-tested and run on the search worker thread. Currency conversions are
// resolved against a caller-supplied rate table (code -> units per 1 USD); the
// network fetch and caching of that table live in the host application.

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace leancast::converter {

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

inline std::wstring Lower(std::wstring value) {
  for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
  return value;
}

inline std::wstring Upper(std::wstring value) {
  for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
  return value;
}

inline std::wstring FormatNumber(double value) {
  if (!std::isfinite(value)) return L"";
  if (std::fabs(value) < 0.0000000001) value = 0.0;
  std::wostringstream stream;
  const double magnitude = std::fabs(value);
  if (magnitude != 0.0 && (magnitude < 0.0001 || magnitude >= 1000000000000.0)) {
    stream << std::setprecision(6) << value;
    return stream.str();
  }
  stream << std::fixed << std::setprecision(6) << value;
  std::wstring out = stream.str();
  while (!out.empty() && out.back() == L'0') out.pop_back();
  if (!out.empty() && out.back() == L'.') out.pop_back();
  if (out == L"-0") out = L"0";
  return out;
}

// A measurement unit identified by its category and factor to the category's
// base unit. Temperature uses a dedicated code instead of a linear factor.
struct Unit {
  std::wstring category;
  double factor = 1.0;
  std::wstring tempCode;  // non-empty only for temperature units
};

inline const std::map<std::wstring, Unit>& UnitTable() {
  static const std::map<std::wstring, Unit> table = [] {
    std::map<std::wstring, Unit> t;
    auto add = [&](std::initializer_list<const wchar_t*> names, const wchar_t* cat, double factor) {
      for (const auto* n : names) t[n] = Unit{cat, factor, L""};
    };
    // Length (base: meter)
    add({L"m", L"meter", L"meters", L"metre", L"metres"}, L"length", 1.0);
    add({L"km", L"kilometer", L"kilometers", L"kilometre"}, L"length", 1000.0);
    add({L"cm", L"centimeter", L"centimeters"}, L"length", 0.01);
    add({L"mm", L"millimeter", L"millimeters"}, L"length", 0.001);
    add({L"mi", L"mile", L"miles"}, L"length", 1609.344);
    add({L"yd", L"yard", L"yards"}, L"length", 0.9144);
    add({L"ft", L"foot", L"feet"}, L"length", 0.3048);
    add({L"inch", L"inches"}, L"length", 0.0254);
    add({L"nmi", L"nauticalmile"}, L"length", 1852.0);
    // Mass (base: gram)
    add({L"g", L"gram", L"grams"}, L"mass", 1.0);
    add({L"kg", L"kilogram", L"kilograms"}, L"mass", 1000.0);
    add({L"mg", L"milligram", L"milligrams"}, L"mass", 0.001);
    add({L"lb", L"lbs", L"pound", L"pounds"}, L"mass", 453.59237);
    add({L"oz", L"ounce", L"ounces"}, L"mass", 28.349523125);
    add({L"t", L"tonne", L"tonnes", L"ton"}, L"mass", 1000000.0);
    add({L"st", L"stone", L"stones"}, L"mass", 6350.29318);
    // Temperature (special handling via tempCode)
    t[L"c"] = t[L"celsius"] = Unit{L"temp", 0.0, L"c"};
    t[L"f"] = t[L"fahrenheit"] = Unit{L"temp", 0.0, L"f"};
    t[L"k"] = t[L"kelvin"] = Unit{L"temp", 0.0, L"k"};
    // Data (base: byte, decimal SI plus binary IEC)
    add({L"b", L"byte", L"bytes"}, L"data", 1.0);
    add({L"kb", L"kilobyte", L"kilobytes"}, L"data", 1000.0);
    add({L"mb", L"megabyte", L"megabytes"}, L"data", 1000000.0);
    add({L"gb", L"gigabyte", L"gigabytes"}, L"data", 1000000000.0);
    add({L"tb", L"terabyte", L"terabytes"}, L"data", 1000000000000.0);
    add({L"kib"}, L"data", 1024.0);
    add({L"mib"}, L"data", 1048576.0);
    add({L"gib"}, L"data", 1073741824.0);
    // Time (base: second)
    add({L"s", L"sec", L"secs", L"second", L"seconds"}, L"time", 1.0);
    add({L"ms", L"millisecond", L"milliseconds"}, L"time", 0.001);
    add({L"min", L"mins", L"minute", L"minutes"}, L"time", 60.0);
    add({L"h", L"hr", L"hrs", L"hour", L"hours"}, L"time", 3600.0);
    add({L"day", L"days"}, L"time", 86400.0);
    add({L"week", L"weeks"}, L"time", 604800.0);
    // Speed (base: meter/second)
    add({L"mps"}, L"speed", 1.0);
    add({L"kmh", L"kph"}, L"speed", 0.277777778);
    add({L"mph"}, L"speed", 0.44704);
    add({L"knot", L"knots"}, L"speed", 0.514444444);
    // Volume (base: liter)
    add({L"l", L"liter", L"liters", L"litre", L"litres"}, L"volume", 1.0);
    add({L"ml", L"milliliter", L"milliliters"}, L"volume", 0.001);
    add({L"gal", L"gallon", L"gallons"}, L"volume", 3.785411784);
    add({L"qt", L"quart", L"quarts"}, L"volume", 0.946352946);
    add({L"pt", L"pint", L"pints"}, L"volume", 0.473176473);
    add({L"cup", L"cups"}, L"volume", 0.2365882365);
    add({L"floz"}, L"volume", 0.0295735296);
    // Area (base: square meter)
    add({L"sqm", L"m2"}, L"area", 1.0);
    add({L"sqkm", L"km2"}, L"area", 1000000.0);
    add({L"sqft", L"ft2"}, L"area", 0.09290304);
    add({L"sqmi", L"mi2"}, L"area", 2589988.110336);
    add({L"acre", L"acres"}, L"area", 4046.8564224);
    add({L"hectare", L"hectares", L"ha"}, L"area", 10000.0);
    // Angle (base: degree)
    add({L"deg", L"degree", L"degrees"}, L"angle", 1.0);
    add({L"rad", L"radian", L"radians"}, L"angle", 57.2957795130823);
    return t;
  }();
  return table;
}

inline double ToCelsius(double value, const std::wstring& code) {
  if (code == L"f") return (value - 32.0) * 5.0 / 9.0;
  if (code == L"k") return value - 273.15;
  return value;
}

inline double FromCelsius(double celsius, const std::wstring& code) {
  if (code == L"f") return celsius * 9.0 / 5.0 + 32.0;
  if (code == L"k") return celsius + 273.15;
  return celsius;
}

inline std::vector<std::wstring> SplitTokens(const std::wstring& text) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (const wchar_t ch : text) {
    if (std::iswspace(ch)) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

inline bool IsConnector(const std::wstring& token) {
  return token == L"to" || token == L"in" || token == L"into" || token == L"as";
}

// True when the token begins with a parseable number (e.g. "5", "-3.2", "10km").
inline bool LeadingNumber(const std::wstring& token) {
  if (token.empty()) return false;
  const wchar_t* begin = token.c_str();
  wchar_t* end = nullptr;
  std::wcstod(begin, &end);
  return end != begin;
}

// Common single-character currency symbols mapped to their ISO-4217 code. Both
// keys and codes are written with \u escapes so the source stays
// encoding-independent. Symbols like "$" are inherently ambiguous; we pick the
// most common interpretation.
inline const std::map<wchar_t, std::wstring>& CurrencySymbols() {
  static const std::map<wchar_t, std::wstring> table = {
      {L'$', L"USD"},        // dollar
      {L'\u20AC', L"EUR"},   // euro
      {L'\u00A3', L"GBP"},   // pound
      {L'\u00A5', L"JPY"},   // yen
      {L'\u20B9', L"INR"},   // rupee
      {L'\u20A9', L"KRW"},   // won
      {L'\u20BD', L"RUB"},   // ruble
      {L'\u20BA', L"TRY"},   // lira
      {L'\u20AA', L"ILS"},   // shekel
      {L'\u0E3F', L"THB"},   // baht
  };
  return table;
}

// Replaces currency symbols with their spaced ISO code so "$5" becomes " USD 5",
// letting the regular tokenizer/parser take over.
inline std::wstring ExpandCurrencySymbols(const std::wstring& text) {
  const auto& symbols = CurrencySymbols();
  std::wstring out;
  out.reserve(text.size());
  for (const wchar_t ch : text) {
    if (const auto it = symbols.find(ch); it != symbols.end()) {
      out.push_back(L' ');
      out += it->second;
      out.push_back(L' ');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// Parses "<number> <fromUnit> [to|in|into|as] <toUnit>" and converts. Also
// accepts a currency-first amount ("USD 5"), "=" as a connector ("USD 5 = GBP"),
// and a lone currency ("USD 5") which converts to defaultCurrency when supplied.
// Returns nullopt for anything that is not a recognized cross-unit/currency
// conversion, leaving plain arithmetic to the calculator.
inline std::optional<Result> TryConvert(std::wstring input, const std::map<std::wstring, double>& rates = {},
                                        const std::wstring& defaultCurrency = L"") {
  const std::wstring original = Trim(std::move(input));
  if (original.empty()) return std::nullopt;

  // Normalize the query so the existing number-first parser can handle it, while
  // keeping `original` intact for display. Expand currency symbols ("$" -> "USD"),
  // treat "=" as a connector, then swap a leading "<currency> <number>" into
  // "<number> <currency>".
  std::wstring normalized = ExpandCurrencySymbols(original);
  for (size_t pos; (pos = normalized.find(L'=')) != std::wstring::npos;) {
    normalized.replace(pos, 1, L" to ");
  }
  {
    std::vector<std::wstring> head = SplitTokens(normalized);
    if (head.size() >= 2 && !LeadingNumber(head[0]) && LeadingNumber(head[1])) {
      std::swap(head[0], head[1]);
      normalized.clear();
      for (size_t i = 0; i < head.size(); ++i) {
        if (i) normalized.push_back(L' ');
        normalized += head[i];
      }
    }
  }

  // Parse the leading numeric amount.
  const wchar_t* begin = normalized.c_str();
  wchar_t* numberEnd = nullptr;
  const double amount = std::wcstod(begin, &numberEnd);
  if (numberEnd == begin || !std::isfinite(amount)) return std::nullopt;

  std::wstring remainder = Trim(std::wstring(numberEnd));
  if (remainder.empty()) return std::nullopt;

  // Normalize arrow connectors into a spaced word so tokenizing is uniform.
  for (const auto* arrow : {L"->", L">"}) {
    size_t pos;
    while ((pos = remainder.find(arrow)) != std::wstring::npos) {
      remainder.replace(pos, std::wcslen(arrow), L" to ");
    }
  }

  const std::vector<std::wstring> tokens = SplitTokens(remainder);
  std::wstring fromToken;
  std::wstring toToken;
  if (tokens.size() == 1) {
    // A lone currency ("5 USD") converts to the caller's locale currency, unless
    // they are the same (which would be a pointless "5 EUR = 5 EUR").
    if (!defaultCurrency.empty() && Upper(tokens[0]) != Upper(defaultCurrency)) {
      fromToken = tokens[0];
      toToken = defaultCurrency;
    }
  } else if (tokens.size() == 2) {
    fromToken = tokens[0];
    toToken = tokens[1];
  } else if (tokens.size() >= 3) {
    // Find a connector that is not the first or last token.
    for (size_t i = 1; i + 1 < tokens.size(); ++i) {
      if (IsConnector(Lower(tokens[i]))) {
        fromToken = tokens[i - 1];
        toToken = tokens[i + 1];
        break;
      }
    }
  }
  if (fromToken.empty() || toToken.empty()) return std::nullopt;

  // Currency conversion takes priority when both tokens are known codes.
  if (!rates.empty()) {
    const std::wstring fromCode = Upper(fromToken);
    const std::wstring toCode = Upper(toToken);
    const auto fromRate = rates.find(fromCode);
    const auto toRate = rates.find(toCode);
    if (fromRate != rates.end() && toRate != rates.end() && fromRate->second > 0.0) {
      const double usd = amount / fromRate->second;
      const double converted = usd * toRate->second;
      const std::wstring display = FormatNumber(converted) + L" " + toCode;
      return Result{original, display, converted};
    }
    // If exactly one side is a currency code, it is an unresolved currency
    // query (e.g. unknown code) rather than a unit conversion.
    if ((fromRate != rates.end()) != (toRate != rates.end())) return std::nullopt;
  }

  const auto& table = UnitTable();
  const auto fromUnit = table.find(Lower(fromToken));
  const auto toUnit = table.find(Lower(toToken));
  if (fromUnit == table.end() || toUnit == table.end()) return std::nullopt;
  if (fromUnit->second.category != toUnit->second.category) return std::nullopt;

  double converted = 0.0;
  if (fromUnit->second.category == L"temp") {
    const double celsius = ToCelsius(amount, fromUnit->second.tempCode);
    converted = FromCelsius(celsius, toUnit->second.tempCode);
  } else {
    if (toUnit->second.factor == 0.0) return std::nullopt;
    converted = amount * fromUnit->second.factor / toUnit->second.factor;
  }

  if (!std::isfinite(converted)) return std::nullopt;
  const std::wstring display = FormatNumber(converted) + L" " + Lower(toToken);
  return Result{original, display, converted};
}

}  // namespace leancast::converter
