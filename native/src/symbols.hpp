#pragma once

#include "core.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <mutex>

namespace leancast::symbols {

struct Symbol {
  std::wstring value;
  std::wstring label;
  std::vector<std::wstring> keywords;
};

inline const std::vector<Symbol>& AllSymbols() {
  static const std::vector<Symbol> kSymbols = {
    {L"\U0001F600", L"Grinning Face", {L"face", L"smile", L"happy", L"grin"}},
    {L"\U0001F642", L"Slightly Smiling Face", {L"face", L"smile", L"happy"}},
    {L"\U0001F601", L"Beaming Face", {L"face", L"smile", L"happy", L"teeth"}},
    {L"\U0001F602", L"Face With Tears of Joy", {L"face", L"laugh", L"lol", L"joy"}},
    {L"\U0001F609", L"Winking Face", {L"face", L"wink"}},
    {L"\U0001F60D", L"Smiling Face With Hearts", {L"face", L"love", L"heart"}},
    {L"\U0001F914", L"Thinking Face", {L"face", L"think", L"hmm"}},
    {L"\U0001F44D", L"Thumbs Up", {L"thumb", L"yes", L"approve", L"like"}},
    {L"\U0001F44E", L"Thumbs Down", {L"thumb", L"no", L"disapprove"}},
    {L"\U0001F44F", L"Clapping Hands", {L"clap", L"applause", L"hands"}},
    {L"\U0001F64F", L"Folded Hands", {L"please", L"thanks", L"pray", L"hands"}},
    {L"\U0001F525", L"Fire", {L"hot", L"flame"}},
    {L"\u2705", L"Check Mark Button", {L"check", L"done", L"success", L"yes"}},
    {L"\u274C", L"Cross Mark", {L"x", L"cross", L"error", L"no"}},
    {L"\u26A0", L"Warning", {L"alert", L"caution", L"warn"}},
    {L"\u2139", L"Information", {L"info", L"note"}},
    {L"\u2192", L"Right Arrow", {L"arrow", L"right", L"next"}},
    {L"\u2190", L"Left Arrow", {L"arrow", L"left", L"back"}},
    {L"\u2191", L"Up Arrow", {L"arrow", L"up"}},
    {L"\u2193", L"Down Arrow", {L"arrow", L"down"}},
    {L"\u2194", L"Left Right Arrow", {L"arrow", L"horizontal"}},
    {L"\u21D2", L"Double Right Arrow", {L"arrow", L"implies", L"right"}},
    {L"\u21D4", L"Double Left Right Arrow", {L"arrow", L"iff", L"equivalent"}},
    {L"\u2022", L"Bullet", {L"bullet", L"dot", L"list"}},
    {L"\u2023", L"Triangular Bullet", {L"bullet", L"triangle", L"list"}},
    {L"\u25E6", L"White Bullet", {L"bullet", L"circle", L"list"}},
    {L"\u2605", L"Black Star", {L"star", L"favorite", L"rating"}},
    {L"\u2606", L"White Star", {L"star", L"favorite", L"rating"}},
    {L"\u2726", L"Black Four Pointed Star", {L"star", L"sparkle"}},
    {L"\u00B1", L"Plus Minus", {L"math", L"plus", L"minus"}},
    {L"\u00D7", L"Multiplication Sign", {L"math", L"multiply", L"times"}},
    {L"\u00F7", L"Division Sign", {L"math", L"divide"}},
    {L"\u2260", L"Not Equal", {L"math", L"neq", L"not"}},
    {L"\u2264", L"Less Than or Equal", {L"math", L"lte", L"less"}},
    {L"\u2265", L"Greater Than or Equal", {L"math", L"gte", L"greater"}},
    {L"\u2248", L"Almost Equal", {L"math", L"approx", L"about"}},
    {L"\u221E", L"Infinity", {L"math", L"infinite"}},
    {L"\u221A", L"Square Root", {L"math", L"root", L"sqrt"}},
    {L"\u03C0", L"Greek Small Letter Pi", {L"math", L"pi", L"greek"}},
    {L"\u03B1", L"Greek Small Letter Alpha", {L"greek", L"alpha"}},
    {L"\u03B2", L"Greek Small Letter Beta", {L"greek", L"beta"}},
    {L"\u03BB", L"Greek Small Letter Lambda", {L"greek", L"lambda"}},
    {L"\u00A9", L"Copyright Sign", {L"legal", L"copyright"}},
    {L"\u00AE", L"Registered Sign", {L"legal", L"registered"}},
    {L"\u2122", L"Trademark Sign", {L"legal", L"trademark", L"tm"}},
    {L"\u00A7", L"Section Sign", {L"legal", L"section"}},
    {L"\u00B6", L"Pilcrow Sign", {L"text", L"paragraph", L"pilcrow"}},
    {L"\u2014", L"Em Dash", {L"text", L"dash", L"punctuation"}},
    {L"\u2013", L"En Dash", {L"text", L"dash", L"punctuation"}},
    {L"\u2026", L"Ellipsis", {L"text", L"dots", L"punctuation"}},
    {L"\u00B0", L"Degree Sign", {L"temperature", L"degree"}},
    {L"\u20AC", L"Euro Sign", {L"currency", L"euro"}},
    {L"\u00A3", L"Pound Sign", {L"currency", L"pound"}},
    {L"\u00A5", L"Yen Sign", {L"currency", L"yen"}},
    {L"\u20B9", L"Rupee Sign", {L"currency", L"rupee"}},
    {L"\u2713", L"Check Mark", {L"check", L"tick", L"done"}},
    {L"\u2717", L"Ballot X", {L"x", L"cross", L"no"}},
  };
  return kSymbols;
}

inline std::vector<leancast::core::SearchItem>* g_SymbolsSearchItems = nullptr;
inline std::mutex g_SymbolsMutex;

inline void FreeSymbolsMemory() {
  std::lock_guard<std::mutex> lock(g_SymbolsMutex);
  if (g_SymbolsSearchItems) {
    delete g_SymbolsSearchItems;
    g_SymbolsSearchItems = nullptr;
  }
}

inline std::vector<Symbol> SearchSymbols(std::wstring query, size_t limit = 32) {
  query = leancast::core::Trim(std::move(query));
  if (!query.empty() && query.front() == L':') {
    query.erase(query.begin());
    query = leancast::core::Trim(std::move(query));
  }

  const auto& all = AllSymbols();
  std::vector<Symbol> out;
  if (query.empty()) {
    for (const auto& symbol : all) {
      out.push_back(symbol);
      if (out.size() >= limit) break;
    }
    return out;
  }

  std::lock_guard<std::mutex> lock(g_SymbolsMutex);
  if (!g_SymbolsSearchItems) {
    g_SymbolsSearchItems = new std::vector<leancast::core::SearchItem>();
    g_SymbolsSearchItems->reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
      leancast::core::SearchItem item;
      item.id = std::to_wstring(i);
      item.kind = L"symbol";
      item.source = L"symbol";
      item.name = all[i].label;
      item.keywords = all[i].keywords;
      item.keywords.push_back(all[i].value);
      g_SymbolsSearchItems->push_back(std::move(item));
    }
  }

  const auto order = leancast::core::Search(query, *g_SymbolsSearchItems);
  for (const auto index : order) {
    out.push_back(all[index]);
    if (out.size() >= limit) break;
  }
  return out;
}

}  // namespace leancast::symbols
