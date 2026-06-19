#include "core.hpp"

#include <cassert>
#include <set>

using leancast::core::ScoreItem;
using leancast::core::ScoreText;
using leancast::core::Search;
using leancast::core::SearchItem;

int main() {
  assert(ScoreText(L"terminal", L"Terminal") == 5000);
  assert(ScoreText(L"term", L"Terminal") > ScoreText(L"trm", L"Terminal"));
  assert(ScoreText(L"calc", L"Calculator") > 0);
  assert(ScoreText(L"xyz", L"Calculator") < 0);

  SearchItem terminal;
  terminal.id = L"terminal";
  terminal.kind = L"app";
  terminal.source = L"alias";
  terminal.name = L"Windows Terminal";
  terminal.keywords = {L"wt", L"shell", L"console", L"powershell"};

  SearchItem notepad;
  notepad.id = L"notepad";
  notepad.kind = L"app";
  notepad.source = L"shortcut";
  notepad.name = L"Notepad";

  SearchItem window;
  window.id = L"hwnd:1";
  window.kind = L"window";
  window.name = L"main.cpp - Visual Studio";
  window.processName = L"devenv";

  std::vector<SearchItem> items = {notepad, terminal, window};
  const auto shell = Search(L"shell", items);
  assert(!shell.empty() && shell.front() == 1);

  const auto cpp = Search(L"studio", items);
  assert(!cpp.empty() && cpp.front() == 2);

  const double base = ScoreItem(L"notepad", notepad, {});
  const double recent = ScoreItem(L"notepad", notepad, {L"notepad"});
  assert(recent > base);

  return 0;
}
