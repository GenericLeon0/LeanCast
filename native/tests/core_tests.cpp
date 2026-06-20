#include "calculator.hpp"
#include "core.hpp"
#include "shortcut.hpp"

#include <cassert>
#include <set>

using leancast::core::ScoreItem;
using leancast::core::ScoreText;
using leancast::core::Search;
using leancast::core::SearchItem;
using leancast::calculator::TryEvaluate;
using leancast::shortcut::ParseShortcut;
using leancast::shortcut::PressedModifiers;
using leancast::shortcut::ShortcutRuntime;

namespace {

PressedModifiers Mods(bool ctrl = false, bool alt = false, bool shift = false, bool win = false) {
  return {ctrl, alt, shift, win};
}

void AssertPassOnly(const leancast::shortcut::HookResult& result) {
  assert(!result.consume);
  assert(!result.toggle);
  assert(!result.suppressWinStart);
}

}  // namespace

int main() {
  assert(ScoreText(L"terminal", L"Terminal") == 5000);
  assert(ScoreText(L"term", L"Terminal") > ScoreText(L"trm", L"Terminal"));
  assert(ScoreText(L"calc", L"Calculator") > 0);
  assert(ScoreText(L"xyz", L"Calculator") < 0);

  {
    const auto simple = TryEvaluate(L"9+9");
    assert(simple && simple->display == L"18");

    const auto precedence = TryEvaluate(L"2+3*4");
    assert(precedence && precedence->display == L"14");

    const auto grouped = TryEvaluate(L"(2+3)*4");
    assert(grouped && grouped->display == L"20");

    const auto decimal = TryEvaluate(L"1,5+2.25");
    assert(decimal && decimal->display == L"3.75");

    const auto percent = TryEvaluate(L"50%");
    assert(percent && percent->display == L"0.5");

    assert(!TryEvaluate(L"9+"));
    assert(!TryEvaluate(L"notepad"));
  }

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

  const auto acronym = Search(L"wt", items);
  assert(!acronym.empty() && acronym.front() == 1);

  const auto cpp = Search(L"studio", items);
  assert(!cpp.empty() && cpp.front() == 2);

  const double base = ScoreItem(L"notepad", notepad, {});
  const double recent = ScoreItem(L"notepad", notepad, {L"notepad"});
  assert(recent > base);

  SearchItem plainCode;
  plainCode.id = L"plain-code";
  plainCode.kind = L"app";
  plainCode.name = L"Code";

  SearchItem pinnedCode = plainCode;
  pinnedCode.id = L"pinned-code";
  pinnedCode.pinned = true;
  assert(ScoreItem(L"code", pinnedCode, {}) > ScoreItem(L"code", plainCode, {}));

  SearchItem usedCode = plainCode;
  usedCode.id = L"used-code";
  usedCode.usageCount = 5;
  usedCode.lastUsed = 1000;
  assert(ScoreItem(L"code", usedCode, {}) > ScoreItem(L"code", plainCode, {}));

  {
    const auto super = ParseShortcut(L"Super");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(super, VK_LWIN, true, false, Mods(false, false, false, true)));
    const auto release = runtime.Handle(super, VK_LWIN, false, true, Mods());
    assert(!release.consume);
    assert(release.toggle);
    assert(release.suppressWinStart);
  }

  {
    const auto super = ParseShortcut(L"Super");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(super, VK_LWIN, true, false, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, L'E', true, false, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, L'E', false, true, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, VK_LWIN, false, true, Mods()));
  }

  {
    const auto superSpace = ParseShortcut(L"Super+Space");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(superSpace, VK_LWIN, true, false, Mods(false, false, false, true)));
    const auto open = runtime.Handle(superSpace, VK_SPACE, true, false, Mods(false, false, false, true));
    assert(open.consume);
    assert(open.toggle);
    assert(!open.suppressWinStart);
    const auto repeat = runtime.Handle(superSpace, VK_SPACE, true, false, Mods(false, false, false, true));
    assert(repeat.consume);
    assert(!repeat.toggle);
    AssertPassOnly(runtime.Handle(superSpace, VK_SPACE, false, true, Mods(false, false, false, true)));
  }

  {
    const auto altSpace = ParseShortcut(L"Alt+Space");
    ShortcutRuntime runtime;
    const auto open = runtime.Handle(altSpace, VK_SPACE, true, false, Mods(false, true));
    assert(open.consume);
    assert(open.toggle);

    const auto controlSpace = ParseShortcut(L"Control+Space");
    ShortcutRuntime controlRuntime;
    const auto controlOpen = controlRuntime.Handle(controlSpace, VK_SPACE, true, false, Mods(true));
    assert(controlOpen.consume);
    assert(controlOpen.toggle);

    const auto alt = ParseShortcut(L"Alt");
    ShortcutRuntime altRuntime;
    AssertPassOnly(altRuntime.Handle(alt, VK_MENU, true, false, Mods(false, true)));
    const auto altRelease = altRuntime.Handle(alt, VK_MENU, false, true, Mods());
    assert(!altRelease.consume);
    assert(altRelease.toggle);
    assert(!altRelease.suppressWinStart);
  }

  return 0;
}
