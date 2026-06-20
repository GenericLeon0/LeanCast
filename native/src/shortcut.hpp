#pragma once

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace leancast::shortcut {

struct ShortcutSpec {
  bool ctrl = false;
  bool alt = false;
  bool shift = false;
  bool win = false;
  UINT vk = 0;
  bool singleModifier = false;
  UINT singleModifierVk = 0;
  bool valid = false;
  std::wstring display;
};

struct PressedModifiers {
  bool ctrl = false;
  bool alt = false;
  bool shift = false;
  bool win = false;
};

struct HookResult {
  bool consume = false;
  bool toggle = false;
  bool suppressWinStart = false;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

inline UINT VkFromName(std::wstring name) {
  name = Lower(Trim(std::move(name)));
  if (name == L"space") return VK_SPACE;
  if (name == L"return" || name == L"enter") return VK_RETURN;
  if (name == L"tab") return VK_TAB;
  if (name == L"up") return VK_UP;
  if (name == L"down") return VK_DOWN;
  if (name == L"left") return VK_LEFT;
  if (name == L"right") return VK_RIGHT;
  if (name == L"home") return VK_HOME;
  if (name == L"end") return VK_END;
  if (name == L"pageup") return VK_PRIOR;
  if (name == L"pagedown") return VK_NEXT;
  if (name == L"insert") return VK_INSERT;
  if (name == L"delete") return VK_DELETE;
  if (name == L"backspace") return VK_BACK;
  if (name == L"escape" || name == L"esc") return VK_ESCAPE;
  if (name == L"plus") return VK_OEM_PLUS;
  if (name == L"-") return VK_OEM_MINUS;
  if (name == L",") return VK_OEM_COMMA;
  if (name == L".") return VK_OEM_PERIOD;
  if (name == L"/") return VK_OEM_2;
  if (name.size() == 1) {
    const wchar_t ch = name[0];
    if (ch >= L'a' && ch <= L'z') return static_cast<UINT>(L'A' + ch - L'a');
    if (ch >= L'0' && ch <= L'9') return static_cast<UINT>(ch);
  }
  if (name.size() >= 2 && name[0] == L'f') {
    const int n = _wtoi(name.c_str() + 1);
    if (n >= 1 && n <= 12) return VK_F1 + n - 1;
  }
  return 0;
}

inline std::wstring KeyName(UINT vk) {
  if (vk >= L'A' && vk <= L'Z') return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= L'0' && vk <= L'9') return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring(vk - VK_F1 + 1);
  switch (vk) {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Return";
    case VK_TAB: return L"Tab";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_BACK: return L"Backspace";
    case VK_ESCAPE: return L"Escape";
    case VK_OEM_PLUS: return L"Plus";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2: return L"/";
    default: return L"";
  }
}

inline bool IsModifier(UINT vk) {
  return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
         vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
         vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
         vk == VK_LWIN || vk == VK_RWIN;
}

inline std::wstring ModifierName(UINT vk) {
  if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return L"Control";
  if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return L"Alt";
  if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return L"Shift";
  if (vk == VK_LWIN || vk == VK_RWIN) return L"Super";
  return L"";
}

inline UINT GenericModifier(UINT vk) {
  if (vk == VK_LCONTROL || vk == VK_RCONTROL) return VK_CONTROL;
  if (vk == VK_LMENU || vk == VK_RMENU) return VK_MENU;
  if (vk == VK_LSHIFT || vk == VK_RSHIFT) return VK_SHIFT;
  if (vk == VK_RWIN) return VK_LWIN;
  return vk;
}

inline bool ModifierPressed(UINT vk) {
  if (vk == VK_CONTROL) return (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
  if (vk == VK_MENU) return (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
  if (vk == VK_SHIFT) return (GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
  if (vk == VK_LWIN) return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
  return false;
}

inline PressedModifiers CurrentPressedModifiers() {
  return {
    ModifierPressed(VK_CONTROL),
    ModifierPressed(VK_MENU),
    ModifierPressed(VK_SHIFT),
    ModifierPressed(VK_LWIN),
  };
}

inline std::wstring FormatShortcut(bool ctrl, bool alt, bool shift, bool win, UINT vk, bool singleModifier = false, UINT singleModifierVk = 0) {
  if (singleModifier) return ModifierName(singleModifierVk);
  std::vector<std::wstring> parts;
  if (ctrl) parts.push_back(L"Control");
  if (alt) parts.push_back(L"Alt");
  if (shift) parts.push_back(L"Shift");
  if (win) parts.push_back(L"Super");
  const auto key = KeyName(vk);
  if (!key.empty()) parts.push_back(key);
  std::wstring out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += L"+";
    out += parts[i];
  }
  return out;
}

inline ShortcutSpec ParseShortcut(const std::wstring& input) {
  ShortcutSpec spec;
  const std::wstring raw = Trim(input);
  if (raw.empty() || Lower(raw) == L"none") {
    spec.display = L"none";
    return spec;
  }

  std::vector<std::wstring> parts;
  std::wstring current;
  for (const wchar_t ch : raw) {
    if (ch == L'+') {
      parts.push_back(Trim(current));
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) parts.push_back(Trim(current));

  if (parts.size() == 1) {
    const std::wstring lower = Lower(parts[0]);
    UINT mod = 0;
    if (lower == L"control" || lower == L"ctrl") mod = VK_CONTROL;
    else if (lower == L"alt") mod = VK_MENU;
    else if (lower == L"shift") mod = VK_SHIFT;
    else if (lower == L"super" || lower == L"win") mod = VK_LWIN;
    if (mod) {
      spec.singleModifier = true;
      spec.singleModifierVk = mod;
      spec.valid = true;
      spec.display = FormatShortcut(false, false, false, false, 0, true, mod);
      return spec;
    }
  }

  bool hasModifier = false;
  for (const auto& part : parts) {
    const std::wstring lower = Lower(part);
    if (lower == L"control" || lower == L"ctrl") {
      spec.ctrl = true;
      hasModifier = true;
    } else if (lower == L"alt") {
      spec.alt = true;
      hasModifier = true;
    } else if (lower == L"shift") {
      spec.shift = true;
      hasModifier = true;
    } else if (lower == L"super" || lower == L"win") {
      spec.win = true;
      hasModifier = true;
    } else {
      spec.vk = VkFromName(part);
    }
  }

  spec.valid = hasModifier && spec.vk != 0;
  spec.display = spec.valid ? FormatShortcut(spec.ctrl, spec.alt, spec.shift, spec.win, spec.vk) : raw;
  return spec;
}

class ShortcutRuntime {
 public:
  HookResult Handle(const ShortcutSpec& shortcut, UINT vk, bool down, bool up, PressedModifiers modifiers) {
    if (!shortcut.valid) return {};

    if (shortcut.singleModifier) {
      const UINT generic = GenericModifier(vk);
      const bool matches = generic == shortcut.singleModifierVk ||
                           (shortcut.singleModifierVk == VK_LWIN && (vk == VK_LWIN || vk == VK_RWIN));
      if (down) {
        if (matches) {
          singleModifierDown_ = true;
          singleModifierChord_ = false;
          return {};
        }
        if (singleModifierDown_) singleModifierChord_ = true;
      } else if (up && matches) {
        HookResult result;
        if (singleModifierDown_ && !singleModifierChord_) {
          result.toggle = true;
          result.suppressWinStart = shortcut.singleModifierVk == VK_LWIN;
        }
        singleModifierDown_ = false;
        singleModifierChord_ = false;
        return result;
      }
      return {};
    }

    if (down && vk == shortcut.vk) {
      const bool exact = modifiers.ctrl == shortcut.ctrl &&
                         modifiers.alt == shortcut.alt &&
                         modifiers.shift == shortcut.shift &&
                         modifiers.win == shortcut.win;
      if (exact) {
        HookResult result{true, !targetKeyDown_, false};
        targetKeyDown_ = true;
        return result;
      }
    } else if (up && vk == shortcut.vk) {
      targetKeyDown_ = false;
    }

    return {};
  }

 private:
  bool singleModifierDown_ = false;
  bool singleModifierChord_ = false;
  bool targetKeyDown_ = false;
};

}  // namespace leancast::shortcut
