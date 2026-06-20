#include "calculator.hpp"
#include "core.hpp"
#include "shortcut.hpp"

#include <windows.h>
#include <cmath>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <propidl.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using leancast::shortcut::CurrentPressedModifiers;
using leancast::shortcut::FormatShortcut;
using leancast::shortcut::GenericModifier;
using leancast::shortcut::IsModifier;
using leancast::shortcut::KeyName;
using leancast::shortcut::ModifierPressed;
using leancast::shortcut::ParseShortcut;
using leancast::shortcut::ShortcutRuntime;
using leancast::shortcut::ShortcutSpec;

namespace {

constexpr int IDI_APP_ICON = 101;
constexpr wchar_t kWindowClass[] = L"LeanCastNativeWindow";
constexpr wchar_t kMutexName[] = L"LeanCastNativeSingleInstance";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOW_SEARCH = WM_APP + 2;
constexpr UINT WM_ICON_READY = WM_APP + 3;
constexpr UINT WM_REBUILD_RESULTS = WM_APP + 4;

constexpr int WIN_WIDTH = 720;
constexpr int WIN_HEIGHT = 470;
constexpr int COMPACT_BASE_HEIGHT = 60;
constexpr int RECENT_LIMIT = 8;
constexpr int MAX_RESULTS = 200;
constexpr int MIN_OVERLAY_WIDTH = 560;
constexpr int MAX_OVERLAY_WIDTH = 980;
constexpr int MIN_RESULTS = 25;
constexpr int MAX_RESULT_SETTING = 400;

enum class View {
  Search,
  Settings,
};

enum class LaunchType {
  Shortcut,
  Exe,
  AppsFolder,
};

enum class HitType {
  Result,
  Gear,
  Back,
  RecordShortcut,
  SaveShortcut,
  ClearShortcut,
  CompactToggle,
  AccentToggle,
  AccentColor,
  StartupToggle,
  ShowWindowsToggle,
  ShowStoreAppsToggle,
  ClearRecents,
  ClearIconCache,
  OverlayWidthDown,
  OverlayWidthUp,
  MaxResultsDown,
  MaxResultsUp,
};

enum class CommandKind {
  Settings,
  Quit,
  Restart,
  RefreshApps,
  ClearIconCache,
  ClearRecents,
  OpenDataFolder,
};

enum class ActionKind {
  None,
  Open,
  RunAsAdmin,
  OpenLocation,
  CopyPath,
  Pin,
  Unpin,
  Hide,
  Unhide,
  Switch,
  Minimize,
  MaximizeRestore,
  CloseWindow,
};

struct RectF {
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
};

struct HitTarget {
  RectF rect;
  HitType type = HitType::Result;
  int index = -1;
};

struct Settings {
  std::wstring shortcut = L"Alt+Space";
  std::vector<std::wstring> recentApps;
  std::vector<std::wstring> pinnedApps;
  std::vector<std::wstring> hiddenApps;
  std::map<std::wstring, std::wstring> appAliases;
  struct UsageStat {
    int launches = 0;
    long long lastUsed = 0;
  };
  std::map<std::wstring, UsageStat> usageStats;
  bool compactMode = false;
  bool syncAccentColor = true;
  std::wstring customAccentColor = L"#5b6cff";
  bool startOnStartup = false;
  int overlayWidth = WIN_WIDTH;
  int maxResults = MAX_RESULTS;
  bool showOpenWindows = true;
  bool showStoreApps = true;
};

struct ShortcutInfo {
  std::wstring target;
  std::wstring args;
  std::wstring cwd;
  std::wstring iconPath;
  int iconIndex = 0;
};

struct AppEntry {
  std::wstring id;
  std::wstring name;
  std::wstring path;
  std::wstring source;
  LaunchType launchType = LaunchType::Shortcut;
  std::wstring launchTarget;
  std::wstring targetPath;
  std::wstring args;
  std::wstring cwd;
  std::wstring appUserModelId;
  std::wstring iconKey;
  bool adminSupported = false;
  bool systemEssential = false;
  std::vector<std::wstring> keywords;
};

struct WindowEntry {
  DWORD pid = 0;
  HWND hwnd = nullptr;
  std::wstring name;
  std::wstring exe;
  std::wstring processName;
  std::wstring iconKey;
};

struct DisplayItem {
  bool isWindow = false;
  bool isCommand = false;
  bool isAction = false;
  bool isCalculator = false;
  AppEntry app;
  WindowEntry window;
  CommandKind command = CommandKind::Settings;
  ActionKind action = ActionKind::None;
  bool actionTargetIsWindow = false;
  AppEntry actionApp;
  WindowEntry actionWindow;
  std::wstring commandName;
  std::wstring commandDetail;
  std::vector<std::wstring> commandKeywords;
  std::wstring calculationExpression;
  std::wstring calculationResult;

  std::wstring Key() const {
    if (isCalculator) return L"calc:" + calculationExpression;
    if (isCommand) return L"cmd:" + std::to_wstring(static_cast<int>(command));
    if (isAction) return L"act:" + std::to_wstring(static_cast<int>(action)) + L":" +
                         (actionTargetIsWindow ? std::to_wstring(reinterpret_cast<uintptr_t>(actionWindow.hwnd))
                                               : (!actionApp.id.empty() ? actionApp.id : actionApp.path));
    if (isWindow) return L"win:" + std::to_wstring(reinterpret_cast<uintptr_t>(window.hwnd));
    return !app.id.empty() ? app.id : app.path;
  }

  std::wstring Name() const {
    if (isCalculator) return calculationResult;
    if (isCommand) return commandName;
    if (isAction) return commandName;
    return isWindow ? window.name : app.name;
  }

  std::wstring IconKey() const {
    if (isCalculator) return L"";
    if (isAction) return actionTargetIsWindow ? (!actionWindow.iconKey.empty() ? actionWindow.iconKey : actionWindow.exe)
                                              : (!actionApp.iconKey.empty() ? actionApp.iconKey : actionApp.path);
    if (isCommand) return L"";
    return isWindow ? (!window.iconKey.empty() ? window.iconKey : window.exe)
                    : (!app.iconKey.empty() ? app.iconKey : app.path);
  }
};

struct Section {
  std::wstring title;
  std::vector<DisplayItem> items;
};

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return L"";
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
  return out;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return "";
  const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string out(needed, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

std::wstring Trim(std::wstring value) {
  return leancast::core::Trim(std::move(value));
}

bool StartsWith(const std::wstring& value, const std::wstring& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::wstring BaseNameNoExt(const std::wstring& path) {
  std::filesystem::path p(path);
  return p.stem().wstring();
}

std::wstring CleanName(const std::wstring& value) {
  std::wstring name = Trim(value);
  if (Lower(name).ends_with(L".lnk")) name.resize(name.size() - 4);
  return name;
}

std::wstring NameKey(const std::wstring& value) {
  return Lower(CleanName(value));
}

bool ShouldSkipName(const std::wstring& value) {
  const std::wstring name = NameKey(value);
  static const wchar_t* prefixes[] = {
    L"uninstall", L"deinstall", L"readme", L"hilfe", L"help", L"website", L"homepage",
  };
  for (const auto* prefix : prefixes) {
    if (StartsWith(name, prefix)) return true;
  }
  return false;
}

std::vector<std::wstring> SplitWords(const std::wstring& value) {
  std::vector<std::wstring> out;
  std::wstring current;
  for (const wchar_t ch : Lower(value)) {
    if (std::iswalnum(ch)) {
      current.push_back(ch);
    } else if (!current.empty()) {
      out.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

std::vector<std::wstring> UniqueKeywords(const std::vector<std::wstring>& values) {
  std::set<std::wstring> seen;
  std::vector<std::wstring> out;
  for (const auto& value : values) {
    for (const auto& word : SplitWords(value)) {
      if (word.size() < 2 || seen.contains(word)) continue;
      seen.insert(word);
      out.push_back(word);
    }
  }
  return out;
}

std::vector<std::wstring> KeywordsFor(const std::wstring& name, const std::wstring& target, const std::wstring& appId) {
  std::vector<std::wstring> groups = {name, BaseNameNoExt(target), appId};
  const std::wstring lower = Lower(name + L" " + target + L" " + appId);
  if (lower.find(L"terminal") != std::wstring::npos || lower.find(L"wt.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"wt", L"shell", L"console", L"cmd", L"powershell"});
  }
  if (lower.find(L"command prompt") != std::wstring::npos || lower.find(L"cmd.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"cmd", L"console", L"terminal"});
  }
  if (lower.find(L"powershell") != std::wstring::npos || lower.find(L"pwsh.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"pwsh", L"shell", L"terminal"});
  }
  if (lower.find(L"settings") != std::wstring::npos || lower.find(L"immersivecontrolpanel") != std::wstring::npos) {
    groups.insert(groups.end(), {L"preferences", L"control panel", L"system"});
  }
  if (lower.find(L"calculator") != std::wstring::npos || lower.find(L"calc.exe") != std::wstring::npos) {
    groups.push_back(L"calc");
  }
  return UniqueKeywords(groups);
}

bool IsSystemEssentialName(const std::wstring& name) {
  static const std::set<std::wstring> names = {
    L"terminal", L"windows terminal", L"command prompt", L"windows powershell",
    L"powershell 7 (x64)", L"settings", L"calculator",
  };
  return names.contains(NameKey(name));
}

long long UnixNow() {
  return static_cast<long long>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::wstring PrimaryAppId(const AppEntry& app) {
  if (!app.id.empty()) return app.id;
  if (!app.path.empty()) return app.path;
  return app.launchTarget;
}

std::vector<std::wstring> AppKeys(const AppEntry& app) {
  std::vector<std::wstring> keys;
  auto add = [&](const std::wstring& key) {
    if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  };
  add(app.id);
  add(app.path);
  add(app.launchTarget);
  add(app.targetPath);
  return keys;
}

bool ContainsValue(const std::vector<std::wstring>& values, const std::wstring& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsAnyAppKey(const std::vector<std::wstring>& values, const AppEntry& app) {
  for (const auto& key : AppKeys(app)) {
    if (ContainsValue(values, key)) return true;
  }
  return false;
}

void RemoveValue(std::vector<std::wstring>& values, const std::wstring& value) {
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

bool IsStoreLikeSource(const std::wstring& source) {
  return source == L"appx" || source == L"start" || source == L"alias";
}

DisplayItem AppDisplay(const AppEntry& app) {
  DisplayItem item;
  item.app = app;
  return item;
}

DisplayItem WindowDisplay(const WindowEntry& window) {
  DisplayItem item;
  item.isWindow = true;
  item.window = window;
  return item;
}

DisplayItem CommandDisplay(CommandKind command, std::wstring name, std::wstring detail, std::vector<std::wstring> keywords) {
  DisplayItem item;
  item.isCommand = true;
  item.command = command;
  item.commandName = std::move(name);
  item.commandDetail = std::move(detail);
  item.commandKeywords = std::move(keywords);
  return item;
}

DisplayItem ActionDisplay(ActionKind action, std::wstring name, std::wstring detail, const DisplayItem& target) {
  DisplayItem item;
  item.isAction = true;
  item.action = action;
  item.commandKeywords = {name, detail};
  item.commandName = std::move(name);
  item.commandDetail = std::move(detail);
  item.actionTargetIsWindow = target.isWindow;
  item.actionApp = target.app;
  item.actionWindow = target.window;
  return item;
}

DisplayItem CalculatorDisplay(const leancast::calculator::Result& result) {
  DisplayItem item;
  item.isCalculator = true;
  item.calculationExpression = result.expression;
  item.calculationResult = result.display;
  item.commandDetail = L"Calculator result - " + result.expression;
  item.commandKeywords = {L"calculator", L"calc", result.expression, result.display};
  return item;
}

bool PointInRect(const RectF& rect, float x, float y) {
  return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

D2D1_RECT_F ToD2D(const RectF& rect) {
  return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
    out = raw;
  }
  CoTaskMemFree(raw);
  return out;
}

std::filesystem::path UserDataPath() {
  std::filesystem::path root = KnownFolderPath(FOLDERID_RoamingAppData);
  if (root.empty()) {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    root = buf;
  }
  root /= L"LeanCast";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return root;
}

std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find('"', pos);
  if (pos == std::string::npos) return std::nullopt;
  ++pos;
  std::string out;
  bool escaped = false;
  for (; pos < json.size(); ++pos) {
    const char ch = json[pos];
    if (escaped) {
      if (ch == 'n') out.push_back('\n');
      else out.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return out;
    out.push_back(ch);
  }
  return std::nullopt;
}

bool JsonBool(const std::string& json, const std::string& key, bool fallback) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  const size_t value = json.find_first_not_of(" \t\r\n", pos + 1);
  if (value == std::string::npos) return fallback;
  if (json.compare(value, 4, "true") == 0) return true;
  if (json.compare(value, 5, "false") == 0) return false;
  return fallback;
}

int JsonInt(const std::string& json, const std::string& key, int fallback) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  const size_t value = json.find_first_not_of(" \t\r\n", pos + 1);
  if (value == std::string::npos) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(json.c_str() + value, &end, 10);
  if (end == json.c_str() + value) return fallback;
  return static_cast<int>(parsed);
}

long long JsonLongLong(const std::string& json, const std::string& key, long long fallback) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  const size_t value = json.find_first_not_of(" \t\r\n", pos + 1);
  if (value == std::string::npos) return fallback;
  char* end = nullptr;
  const long long parsed = std::strtoll(json.c_str() + value, &end, 10);
  if (end == json.c_str() + value) return fallback;
  return parsed;
}

std::vector<std::wstring> JsonStringArray(const std::string& json, const std::string& key) {
  std::vector<std::wstring> out;
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return out;
  pos = json.find('[', pos);
  if (pos == std::string::npos) return out;
  const size_t end = json.find(']', pos);
  if (end == std::string::npos) return out;
  std::string slice = json.substr(pos, end - pos + 1);
  size_t cursor = 0;
  while (true) {
    cursor = slice.find('"', cursor);
    if (cursor == std::string::npos) break;
    ++cursor;
    std::string value;
    bool escaped = false;
    for (; cursor < slice.size(); ++cursor) {
      const char ch = slice[cursor];
      if (escaped) {
        value.push_back(ch);
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        out.push_back(Utf8ToWide(value));
        ++cursor;
        break;
      } else {
        value.push_back(ch);
      }
    }
  }
  return out;
}

std::optional<std::string> JsonObjectSlice(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find('{', pos);
  if (pos == std::string::npos) return std::nullopt;

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = pos; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '{') {
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) return json.substr(pos, i - pos + 1);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ReadJsonString(const std::string& text, size_t& cursor) {
  cursor = text.find('"', cursor);
  if (cursor == std::string::npos) return std::nullopt;
  ++cursor;
  std::string value;
  bool escaped = false;
  for (; cursor < text.size(); ++cursor) {
    const char ch = text[cursor];
    if (escaped) {
      if (ch == 'n') value.push_back('\n');
      else value.push_back(ch);
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      ++cursor;
      return value;
    } else {
      value.push_back(ch);
    }
  }
  return std::nullopt;
}

std::map<std::wstring, std::wstring> JsonStringObject(const std::string& json, const std::string& key) {
  std::map<std::wstring, std::wstring> out;
  const auto slice = JsonObjectSlice(json, key);
  if (!slice) return out;

  size_t cursor = 1;
  while (cursor < slice->size()) {
    auto rawKey = ReadJsonString(*slice, cursor);
    if (!rawKey) break;
    cursor = slice->find(':', cursor);
    if (cursor == std::string::npos) break;
    ++cursor;
    auto rawValue = ReadJsonString(*slice, cursor);
    if (!rawValue) break;
    out[Utf8ToWide(*rawKey)] = Utf8ToWide(*rawValue);
    cursor = slice->find(',', cursor);
    if (cursor == std::string::npos) break;
    ++cursor;
  }
  return out;
}

std::map<std::wstring, Settings::UsageStat> JsonUsageStats(const std::string& json, const std::string& key) {
  std::map<std::wstring, Settings::UsageStat> out;
  const auto slice = JsonObjectSlice(json, key);
  if (!slice) return out;

  size_t cursor = 1;
  while (cursor < slice->size()) {
    auto rawKey = ReadJsonString(*slice, cursor);
    if (!rawKey) break;
    const size_t valueStart = slice->find('{', cursor);
    if (valueStart == std::string::npos) break;
    int depth = 0;
    size_t valueEnd = std::string::npos;
    for (size_t i = valueStart; i < slice->size(); ++i) {
      if ((*slice)[i] == '{') ++depth;
      else if ((*slice)[i] == '}') {
        --depth;
        if (depth == 0) {
          valueEnd = i;
          break;
        }
      }
    }
    if (valueEnd == std::string::npos) break;
    const std::string statJson = slice->substr(valueStart, valueEnd - valueStart + 1);
    out[Utf8ToWide(*rawKey)] = {
      JsonInt(statJson, "launches", 0),
      JsonLongLong(statJson, "lastUsed", 0),
    };
    cursor = slice->find(',', valueEnd);
    if (cursor == std::string::npos) break;
    ++cursor;
  }
  return out;
}

std::string JsonEscape(const std::wstring& value) {
  std::string in = WideToUtf8(value);
  std::string out;
  for (const char ch : in) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

void WriteStringArray(std::ofstream& file, const std::vector<std::wstring>& values) {
  file << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) file << ", ";
    file << "\"" << JsonEscape(values[i]) << "\"";
  }
  file << "]";
}

void WriteStringObject(std::ofstream& file, const std::map<std::wstring, std::wstring>& values) {
  file << "{";
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) file << ", ";
    first = false;
    file << "\"" << JsonEscape(key) << "\": \"" << JsonEscape(value) << "\"";
  }
  file << "}";
}

void WriteUsageStats(std::ofstream& file, const std::map<std::wstring, Settings::UsageStat>& values) {
  file << "{";
  bool first = true;
  for (const auto& [key, stat] : values) {
    if (!first) file << ", ";
    first = false;
    file << "\"" << JsonEscape(key) << "\": {\"launches\": " << stat.launches
         << ", \"lastUsed\": " << stat.lastUsed << "}";
  }
  file << "}";
}

std::filesystem::path SettingsPath() {
  return UserDataPath() / L"settings.json";
}

void SetStartOnStartup(bool enable) {
  HKEY hKey = nullptr;
  LSTATUS status = RegOpenKeyExW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      0,
      KEY_WRITE,
      &hKey
  );
  if (status == ERROR_SUCCESS) {
    if (enable) {
      wchar_t exePath[MAX_PATH];
      GetModuleFileNameW(nullptr, exePath, MAX_PATH);
      std::wstring pathStr = L"\"" + std::wstring(exePath) + L"\"";
      RegSetValueExW(
          hKey,
          L"LeanCast",
          0,
          REG_SZ,
          reinterpret_cast<const BYTE*>(pathStr.c_str()),
          static_cast<DWORD>((pathStr.length() + 1) * sizeof(wchar_t))
      );
    } else {
      RegDeleteValueW(hKey, L"LeanCast");
    }
    RegCloseKey(hKey);
  }
}

Settings LoadSettings() {
  Settings settings;
  std::ifstream file(SettingsPath(), std::ios::binary);
  if (!file) return settings;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string json = buffer.str();
  if (auto shortcut = JsonString(json, "shortcut")) settings.shortcut = Utf8ToWide(*shortcut);
  settings.recentApps = JsonStringArray(json, "recentApps");
  settings.pinnedApps = JsonStringArray(json, "pinnedApps");
  settings.hiddenApps = JsonStringArray(json, "hiddenApps");
  settings.appAliases = JsonStringObject(json, "appAliases");
  settings.usageStats = JsonUsageStats(json, "usageStats");
  settings.compactMode = JsonBool(json, "compactMode", settings.compactMode);
  settings.syncAccentColor = JsonBool(json, "syncAccentColor", settings.syncAccentColor);
  if (auto custom = JsonString(json, "customAccentColor")) settings.customAccentColor = Utf8ToWide(*custom);
  settings.startOnStartup = JsonBool(json, "startOnStartup", settings.startOnStartup);
  settings.overlayWidth = std::clamp(JsonInt(json, "overlayWidth", settings.overlayWidth), MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
  settings.maxResults = std::clamp(JsonInt(json, "maxResults", settings.maxResults), MIN_RESULTS, MAX_RESULT_SETTING);
  settings.showOpenWindows = JsonBool(json, "showOpenWindows", settings.showOpenWindows);
  settings.showStoreApps = JsonBool(json, "showStoreApps", settings.showStoreApps);
  return settings;
}

void SaveSettings(const Settings& settings) {
  const auto path = SettingsPath();
  auto temp = path;
  temp += L".tmp";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file) return;
  file << "{\n";
  file << "  \"shortcut\": \"" << JsonEscape(settings.shortcut) << "\",\n";
  file << "  \"recentApps\": ";
  WriteStringArray(file, settings.recentApps);
  file << ",\n";
  file << "  \"pinnedApps\": ";
  WriteStringArray(file, settings.pinnedApps);
  file << ",\n";
  file << "  \"hiddenApps\": ";
  WriteStringArray(file, settings.hiddenApps);
  file << ",\n";
  file << "  \"appAliases\": ";
  WriteStringObject(file, settings.appAliases);
  file << ",\n";
  file << "  \"usageStats\": ";
  WriteUsageStats(file, settings.usageStats);
  file << ",\n";
  file << "  \"compactMode\": " << (settings.compactMode ? "true" : "false") << ",\n";
  file << "  \"syncAccentColor\": " << (settings.syncAccentColor ? "true" : "false") << ",\n";
  file << "  \"customAccentColor\": \"" << JsonEscape(settings.customAccentColor) << "\",\n";
  file << "  \"startOnStartup\": " << (settings.startOnStartup ? "true" : "false") << ",\n";
  file << "  \"overlayWidth\": " << std::clamp(settings.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH) << ",\n";
  file << "  \"maxResults\": " << std::clamp(settings.maxResults, MIN_RESULTS, MAX_RESULT_SETTING) << ",\n";
  file << "  \"showOpenWindows\": " << (settings.showOpenWindows ? "true" : "false") << ",\n";
  file << "  \"showStoreApps\": " << (settings.showStoreApps ? "true" : "false") << "\n";
  file << "}\n";
  file.close();

  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
  }
}

COLORREF ColorRefFromHex(const std::wstring& hex, COLORREF fallback = RGB(0x5b, 0x6c, 0xff)) {
  if (hex.size() != 7 || hex[0] != L'#') return fallback;
  const int r = std::wcstol(hex.substr(1, 2).c_str(), nullptr, 16);
  const int g = std::wcstol(hex.substr(3, 2).c_str(), nullptr, 16);
  const int b = std::wcstol(hex.substr(5, 2).c_str(), nullptr, 16);
  return RGB(r, g, b);
}

std::wstring HexFromColorRef(COLORREF color) {
  wchar_t buf[16]{};
  swprintf_s(buf, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
  return buf;
}

D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0f) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

D2D1_COLOR_F Mix(COLORREF a, COLORREF b, float amountA, float alpha = 1.0f) {
  const float r = (GetRValue(a) * amountA + GetRValue(b) * (1.0f - amountA)) / 255.0f;
  const float g = (GetGValue(a) * amountA + GetGValue(b) * (1.0f - amountA)) / 255.0f;
  const float bl = (GetBValue(a) * amountA + GetBValue(b) * (1.0f - amountA)) / 255.0f;
  return D2D1::ColorF(r, g, bl, alpha);
}

uint64_t Fnv1a(const std::wstring& value) {
  uint64_t hash = 1469598103934665603ull;
  for (const wchar_t ch : value) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::filesystem::path IconCacheDir() {
  auto dir = UserDataPath() / L"icon-cache-native";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path IconCachePath(const std::wstring& key) {
  wchar_t buf[32]{};
  swprintf_s(buf, L"%016llx.png", static_cast<unsigned long long>(Fnv1a(Lower(key))));
  return IconCacheDir() / buf;
}

bool LoadShortcut(const std::wstring& lnkPath, ShortcutInfo& info) {
  ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
  ComPtr<IPersistFile> persist;
  if (FAILED(link.As(&persist))) return false;
  if (FAILED(persist->Load(lnkPath.c_str(), STGM_READ))) return false;

  wchar_t target[MAX_PATH]{};
  WIN32_FIND_DATAW findData{};
  if (SUCCEEDED(link->GetPath(target, MAX_PATH, &findData, SLGP_RAWPATH))) info.target = target;

  wchar_t args[4096]{};
  if (SUCCEEDED(link->GetArguments(args, 4096))) info.args = args;

  wchar_t cwd[MAX_PATH]{};
  if (SUCCEEDED(link->GetWorkingDirectory(cwd, MAX_PATH))) info.cwd = cwd;

  wchar_t icon[MAX_PATH]{};
  int iconIndex = 0;
  if (SUCCEEDED(link->GetIconLocation(icon, MAX_PATH, &iconIndex))) {
    info.iconPath = icon;
    info.iconIndex = iconIndex;
  }
  return true;
}

std::wstring FindWindowsAppAlias(const std::wstring& fileName) {
  wchar_t local[MAX_PATH]{};
  GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (!local[0]) return L"";
  std::filesystem::path candidate = std::filesystem::path(local) / L"Microsoft" / L"WindowsApps" / fileName;
  std::error_code ec;
  return std::filesystem::exists(candidate, ec) ? candidate.wstring() : L"";
}

std::wstring ProcessPath(DWORD pid) {
  std::wstring out;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return out;
  wchar_t buf[32768]{};
  DWORD size = static_cast<DWORD>(std::size(buf));
  if (QueryFullProcessImageNameW(process, 0, buf, &size)) out.assign(buf, size);
  CloseHandle(process);
  return out;
}

std::wstring ProcessNameFromPath(const std::wstring& path) {
  if (path.empty()) return L"";
  return std::filesystem::path(path).stem().wstring();
}

std::wstring WindowTitle(HWND hwnd) {
  const int len = GetWindowTextLengthW(hwnd);
  if (len <= 0) return L"";
  std::wstring out(len + 1, L'\0');
  const int copied = GetWindowTextW(hwnd, out.data(), len + 1);
  out.resize(std::max(0, copied));
  return out;
}

bool IsRealTopLevelWindow(HWND hwnd) {
  if (!IsWindowVisible(hwnd)) return false;
  if (WindowTitle(hwnd).empty()) return false;
  const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) return false;
  const HWND owner = GetWindow(hwnd, GW_OWNER);
  if (owner && !(exStyle & WS_EX_APPWINDOW)) return false;
  return true;
}

struct EnumWindowContext {
  HWND own = nullptr;
  std::vector<WindowEntry> windows;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  auto* ctx = reinterpret_cast<EnumWindowContext*>(lParam);
  if (hwnd == ctx->own || !IsRealTopLevelWindow(hwnd)) return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return TRUE;

  WindowEntry entry;
  entry.pid = pid;
  entry.hwnd = hwnd;
  entry.name = WindowTitle(hwnd);
  entry.exe = ProcessPath(pid);
  entry.processName = ProcessNameFromPath(entry.exe);
  entry.iconKey = entry.exe;

  const std::wstring proc = Lower(entry.processName);
  if (proc == L"leancast" || entry.name.find(L"LeanCast") != std::wstring::npos) return TRUE;
  ctx->windows.push_back(std::move(entry));
  return TRUE;
}

std::vector<WindowEntry> ListWindows(HWND own) {
  EnumWindowContext ctx;
  ctx.own = own;
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
  return ctx.windows;
}

void FocusWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;
  if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
}

class LeanCastApp;
LeanCastApp* g_app = nullptr;

class LeanCastApp {
 public:
  explicit LeanCastApp(HINSTANCE instance, std::wstring cmdLine)
      : instance_(instance), cmdLine_(std::move(cmdLine)) {
    settings_ = LoadSettings();
    shortcut_ = ParseShortcut(settings_.shortcut);
    SetStartOnStartup(settings_.startOnStartup);
  }

  ~LeanCastApp() {
    stopThreads_ = true;
    if (discoveryThread_.joinable()) {
      discoveryThread_.request_stop();
      discoveryThread_.join();
    }
    StopIconThreads();
    if (hook_) UnhookWindowsHookEx(hook_);
    RemoveTray();
  }

  int Run() {
    g_app = this;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeFactories();
    RegisterWindowClass();
    CreateMainWindow();
    CreateTray();
    InstallHook();
    StartAppDiscovery();

    if (cmdLine_.find(L"--show") != std::wstring::npos) {
      ShowOverlay(View::Search);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    g_app = nullptr;
    CoUninitialize();
    return static_cast<int>(msg.wParam);
  }

  LRESULT LowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(hook_, nCode, wParam, lParam);
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (!kb || (kb->flags & LLKHF_INJECTED)) return CallNextHookEx(hook_, nCode, wParam, lParam);

    const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const UINT vk = kb->vkCode;

    if (recording_) {
      return HandleRecordingKey(vk, down, up);
    }

    const auto result = shortcutRuntime_.Handle(shortcut_, vk, down, up, CurrentPressedModifiers());
    if (result.suppressWinStart) {
      keybd_event(0xE8, 0, 0, 0);
      keybd_event(0xE8, 0, KEYEVENTF_KEYUP, 0);
    }
    if (result.toggle) ToggleOverlay();
    if (result.consume) return 1;
    return CallNextHookEx(hook_, nCode, wParam, lParam);
  }

 private:
  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LeanCastApp* app = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      app = reinterpret_cast<LeanCastApp*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
      app = reinterpret_cast<LeanCastApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  static LRESULT CALLBACK StaticKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return g_app ? g_app->LowLevelKeyboard(nCode, wParam, lParam) : CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_TIMER:
        if (wParam == 1) {
          if (visible_) {
            if (!suppressHide_) {
              HWND foreground = GetForegroundWindow();
              if (foreground && foreground != hwnd_) {
                HideOverlay(false);
                return 0;
              }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
          }
        }
        return 0;
      case WM_CREATE:
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      case WM_PAINT:
        Paint();
        return 0;
      case WM_SIZE:
        if (renderTarget_) {
          renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && visible_ && !suppressHide_) HideOverlay(false);
        return 0;
      case WM_KILLFOCUS:
        if (visible_ && !suppressHide_) HideOverlay(false);
        return 0;
      case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
      case WM_KEYDOWN:
        OnKeyDown(static_cast<UINT>(wParam));
        return 0;
      case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;
      case WM_MOUSEMOVE:
        OnMouseMove(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
      case WM_LBUTTONDOWN:
        OnClick(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
      case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
      case WM_TRAYICON:
        OnTray(lParam);
        return 0;
      case WM_SHOW_SEARCH:
        ShowOverlay(View::Search);
        return 0;
      case WM_ICON_READY:
      case WM_REBUILD_RESULTS:
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void InitializeFactories() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_));
  }

  void RegisterWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.hInstance = instance_;
    wc.lpfnWndProc = StaticWndProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wc);
  }

  void CreateMainWindow() {
    const int width = OverlayWidth();
    hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
      kWindowClass,
      L"LeanCast",
      WS_POPUP,
      -32000,
      -32000,
      width,
      WIN_HEIGHT,
      nullptr,
      nullptr,
      instance_,
      this);

    SetWindowPos(hwnd_, HWND_TOPMOST, -32000, -32000, width, WIN_HEIGHT, SWP_NOACTIVATE | SWP_HIDEWINDOW);
    ApplyRoundedRegion(width, WIN_HEIGHT);

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
  }

  void CreateDeviceResources() {
    if (renderTarget_ || !d2dFactory_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    d2dFactory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
      renderTarget_.GetAddressOf());

    if (!dwriteFactory_) return;
    CreateTextFormat(19.0f, DWRITE_FONT_WEIGHT_NORMAL, inputFormat_);
    CreateTextFormat(15.0f, DWRITE_FONT_WEIGHT_NORMAL, rowFormat_);
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, subFormat_);
    CreateTextFormat(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, sectionFormat_);
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, footerFormat_);
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, footerRightFormat_);
    if (footerRightFormat_) {
      footerRightFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    CreateTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_);
    CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, labelFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, centerFormat_);
    centerFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    centerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }

  void CreateTextFormat(float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
    dwriteFactory_->CreateTextFormat(
      L"Segoe UI",
      nullptr,
      weight,
      DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL,
      size,
      L"",
      out.GetAddressOf());
    if (out) {
      out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
  }

  void CreateTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"LeanCast");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    tray_ = nid;
  }

  void RemoveTray() {
    if (tray_.cbSize) {
      Shell_NotifyIconW(NIM_DELETE, &tray_);
      if (tray_.hIcon) DestroyIcon(tray_.hIcon);
      tray_ = {};
    }
  }

  void InstallHook() {
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, StaticKeyboardProc, GetModuleHandleW(nullptr), 0);
  }

  LRESULT HandleRecordingKey(UINT vk, bool down, bool up) {
    if (down && vk == VK_ESCAPE) {
      recording_ = false;
      pendingShortcut_.clear();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 1;
    }

    if (down) {
      if (IsModifier(vk)) {
        lastModifierPressed_ = GenericModifier(vk);
        otherKeyWhileRecordingModifier_ = false;
        return 1;
      }

      const bool ctrl = ModifierPressed(VK_CONTROL);
      const bool alt = ModifierPressed(VK_MENU);
      const bool shift = ModifierPressed(VK_SHIFT);
      const bool win = ModifierPressed(VK_LWIN);
      if (ctrl || alt || shift || win) {
        const auto key = KeyName(vk);
        if (!key.empty()) {
          pendingShortcut_ = FormatShortcut(ctrl, alt, shift, win, vk);
          recording_ = false;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
      } else if (lastModifierPressed_) {
        otherKeyWhileRecordingModifier_ = true;
      }
      return 1;
    }

    if (up && IsModifier(vk)) {
      const UINT generic = GenericModifier(vk);
      if (lastModifierPressed_ == generic && !otherKeyWhileRecordingModifier_) {
        pendingShortcut_ = FormatShortcut(false, false, false, false, 0, true, generic);
        recording_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      lastModifierPressed_ = 0;
      otherKeyWhileRecordingModifier_ = false;
      return 1;
    }
    return 1;
  }

  void StartAppDiscovery() {
    if (discoveryThread_.joinable()) {
      discoveryThread_.request_stop();
      discoveryThread_.join();
    }

    {
      std::lock_guard lock(dataMutex_);
      appsReady_ = false;
    }
    PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);

    discoveryThread_ = std::jthread([this](std::stop_token stopToken) {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      auto apps = DiscoverApps();
      if (stopToken.stop_requested() || stopThreads_) {
        CoUninitialize();
        return;
      }
      {
        std::lock_guard lock(dataMutex_);
        apps_ = std::move(apps);
        appsReady_ = true;
      }
      if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
      PrecacheIcons(stopToken);
      CoUninitialize();
    });
  }

  std::vector<AppEntry> DiscoverApps() {
    std::vector<AppEntry> apps;
    std::set<std::wstring> ids;
    std::set<std::wstring> names;

    auto addUnique = [&](AppEntry entry) {
      if (entry.id.empty() || entry.name.empty()) return;
      const std::wstring id = Lower(entry.id);
      const std::wstring name = NameKey(entry.name);
      if (ids.contains(id) || names.contains(name)) return;
      ids.insert(id);
      names.insert(name);
      apps.push_back(std::move(entry));
    };

    for (const auto& path : StartMenuShortcutPaths()) {
      auto entry = ShortcutEntry(path);
      if (entry) addUnique(std::move(*entry));
    }

    for (auto& entry : AppsFolderEntries()) {
      addUnique(std::move(entry));
    }

    std::sort(apps.begin(), apps.end(), [](const AppEntry& a, const AppEntry& b) {
      return Lower(a.name) < Lower(b.name);
    });
    return apps;
  }

  std::vector<std::filesystem::path> StartMenuShortcutPaths() {
    std::vector<std::filesystem::path> dirs;
    wchar_t programData[MAX_PATH]{};
    wchar_t appData[MAX_PATH]{};
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (programData[0]) dirs.emplace_back(std::filesystem::path(programData) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs");
    if (appData[0]) dirs.emplace_back(std::filesystem::path(appData) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs");

    std::vector<std::filesystem::path> out;
    for (const auto& dir : dirs) {
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec)) continue;
      for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) continue;
        if (it->is_regular_file(ec) && Lower(it->path().extension().wstring()) == L".lnk") out.push_back(it->path());
      }
    }
    return out;
  }

  std::optional<AppEntry> ShortcutEntry(const std::filesystem::path& path) {
    const std::wstring name = CleanName(path.stem().wstring());
    if (name.empty() || ShouldSkipName(name)) return std::nullopt;

    ShortcutInfo shortcut;
    LoadShortcut(path.wstring(), shortcut);

    AppEntry entry;
    entry.id = path.wstring();
    entry.name = name;
    entry.path = path.wstring();
    entry.source = L"shortcut";
    entry.launchType = LaunchType::Shortcut;
    entry.launchTarget = path.wstring();
    entry.targetPath = shortcut.target;
    entry.args = shortcut.args;
    entry.cwd = shortcut.cwd;
    entry.iconKey = path.wstring();
    entry.adminSupported = true;
    entry.systemEssential = IsSystemEssentialName(name);
    entry.keywords = KeywordsFor(name, shortcut.target, L"");
    return entry;
  }

  std::vector<AppEntry> AppsFolderEntries() {
    std::vector<AppEntry> out;
    PIDLIST_ABSOLUTE appsPidl = nullptr;
    if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &appsPidl, 0, nullptr))) return out;

    ComPtr<IShellFolder> folder;
    if (SUCCEEDED(SHBindToObject(nullptr, appsPidl, nullptr, IID_PPV_ARGS(&folder)))) {
      ComPtr<IEnumIDList> enumList;
      if (SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, enumList.GetAddressOf()))) {
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        while (enumList->Next(1, &child, &fetched) == S_OK) {
          STRRET str{};
          wchar_t nameBuf[512]{};
          if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &str))) {
            StrRetToBufW(&str, child, nameBuf, 512);
          }

          ComPtr<IShellItem2> item;
          PWSTR appIdRaw = nullptr;
          if (SUCCEEDED(SHCreateItemWithParent(appsPidl, folder.Get(), child, IID_PPV_ARGS(&item)))) {
            item->GetString(PKEY_AppUserModel_ID, &appIdRaw);
          }

          const std::wstring name = CleanName(nameBuf);
          const std::wstring appId = appIdRaw ? appIdRaw : L"";
          CoTaskMemFree(appIdRaw);

          if (!name.empty() && !appId.empty() && !ShouldSkipName(name)) {
            const bool terminal = Lower(name).find(L"terminal") != std::wstring::npos;
            const std::wstring terminalAlias = terminal ? FindWindowsAppAlias(L"wt.exe") : L"";

            AppEntry entry;
            entry.id = L"start:" + appId;
            entry.name = name;
            entry.appUserModelId = appId;
            entry.iconKey = L"appsFolder:" + appId;
            entry.systemEssential = IsSystemEssentialName(name);
            entry.keywords = KeywordsFor(name, terminalAlias, appId);

            if (!terminalAlias.empty()) {
              entry.source = L"alias";
              entry.launchType = LaunchType::Exe;
              entry.launchTarget = terminalAlias;
              entry.targetPath = terminalAlias;
              entry.adminSupported = true;
              entry.systemEssential = true;
            } else {
              entry.source = appId.find(L"!") != std::wstring::npos ? L"appx" : L"start";
              entry.launchType = LaunchType::AppsFolder;
              entry.launchTarget = appId;
              entry.adminSupported = false;
            }
            out.push_back(std::move(entry));
          }
          CoTaskMemFree(child);
        }
      }
    }

    CoTaskMemFree(appsPidl);
    return out;
  }

  void PrecacheIcons(std::stop_token stopToken) {
    std::vector<std::wstring> keys;
    {
      std::lock_guard lock(dataMutex_);
      for (const auto& app : apps_) {
        if (!app.iconKey.empty()) keys.push_back(app.iconKey);
      }
    }
    caching_ = true;
    if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
    for (const auto& key : keys) {
      if (stopToken.stop_requested() || stopThreads_) break;
      ResolveIconToCache(key);
    }
    caching_ = false;
    if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
  }

  void RefreshWindows() {
    auto windows = ListWindows(hwnd_);
    {
      std::lock_guard lock(dataMutex_);
      windows_ = std::move(windows);
    }
  }

  std::vector<DisplayItem> BuiltInCommands() const {
    return {
      CommandDisplay(CommandKind::Settings, L"Settings", L"Open LeanCast settings", {L"preferences", L"options", L"shortcut"}),
      CommandDisplay(CommandKind::Quit, L"Quit LeanCast", L"Exit the background launcher", {L"exit", L"close"}),
      CommandDisplay(CommandKind::Restart, L"Restart LeanCast", L"Restart the native app", {L"reload"}),
      CommandDisplay(CommandKind::RefreshApps, L"Refresh App Index", L"Rescan Start Menu and Store apps", {L"rescan", L"reload apps"}),
      CommandDisplay(CommandKind::ClearIconCache, L"Clear Icon Cache", L"Delete cached shell icons", {L"icons", L"cache"}),
      CommandDisplay(CommandKind::ClearRecents, L"Clear Recents", L"Forget recently used apps", {L"history", L"recent apps"}),
      CommandDisplay(CommandKind::OpenDataFolder, L"Open App Data Folder", L"Open the LeanCast data directory", {L"settings json", L"logs", L"cache folder"}),
    };
  }

  std::vector<DisplayItem> ActionsFor(const DisplayItem& target) const {
    std::vector<DisplayItem> actions;
    if (target.isCommand || target.isAction) return actions;

    if (target.isWindow) {
      actions.push_back(ActionDisplay(ActionKind::Switch, L"Switch to Window", L"Focus " + target.window.name, target));
      actions.push_back(ActionDisplay(ActionKind::Minimize, L"Minimize Window", L"Minimize " + target.window.name, target));
      actions.push_back(ActionDisplay(ActionKind::MaximizeRestore, L"Maximize or Restore Window", L"Toggle window state", target));
      actions.push_back(ActionDisplay(ActionKind::CloseWindow, L"Close Window", L"Send close request", target));
      return actions;
    }

    actions.push_back(ActionDisplay(ActionKind::Open, L"Open", L"Launch " + target.app.name, target));
    if (target.app.adminSupported) {
      actions.push_back(ActionDisplay(ActionKind::RunAsAdmin, L"Run as Administrator", L"Launch elevated", target));
    }
    if (!target.app.path.empty() || !target.app.targetPath.empty()) {
      actions.push_back(ActionDisplay(ActionKind::OpenLocation, L"Open File Location", L"Show app shortcut or target", target));
      actions.push_back(ActionDisplay(ActionKind::CopyPath, L"Copy Path", L"Copy app shortcut or target path", target));
    }
    if (ContainsAnyAppKey(settings_.pinnedApps, target.app)) {
      actions.push_back(ActionDisplay(ActionKind::Unpin, L"Unpin", L"Remove from pinned apps", target));
    } else {
      actions.push_back(ActionDisplay(ActionKind::Pin, L"Pin", L"Keep near the top of results", target));
    }
    if (ContainsAnyAppKey(settings_.hiddenApps, target.app)) {
      actions.push_back(ActionDisplay(ActionKind::Unhide, L"Unhide", L"Show in launcher results", target));
    } else {
      actions.push_back(ActionDisplay(ActionKind::Hide, L"Hide", L"Remove from launcher results", target));
    }
    return actions;
  }

  void BuildSections() {
    std::vector<AppEntry> apps;
    std::vector<WindowEntry> windows;
    {
      std::lock_guard lock(dataMutex_);
      apps = apps_;
      windows = windows_;
    }

    std::vector<Section> sections;
    std::set<std::wstring> used;
    auto take = [&](const std::vector<DisplayItem>& items, size_t limit = SIZE_MAX) {
      std::vector<DisplayItem> out;
      for (const auto& item : items) {
        const auto key = item.Key();
        if (key.empty() || used.contains(key)) continue;
        used.insert(key);
        out.push_back(item);
        if (out.size() >= limit) break;
      }
      return out;
    };
    auto addSection = [&](std::wstring title, std::vector<DisplayItem> items) {
      if (!items.empty()) sections.push_back({std::move(title), std::move(items)});
    };

    const bool empty = Trim(query_).empty();
    if (settings_.compactMode && empty && view_ == View::Search && !actionMode_) {
      sections_.clear();
      flatItems_.clear();
      selected_ = 0;
      ApplyWindowSize();
      return;
    }

    std::vector<DisplayItem> appItems;
    for (const auto& app : apps) {
      if (ContainsAnyAppKey(settings_.hiddenApps, app)) continue;
      if (!settings_.showStoreApps && IsStoreLikeSource(app.source)) continue;
      appItems.push_back(AppDisplay(app));
    }
    std::vector<DisplayItem> windowItems;
    if (settings_.showOpenWindows) {
      for (const auto& window : windows) windowItems.push_back(WindowDisplay(window));
    }
    const auto commandItems = BuiltInCommands();

    if (actionMode_) {
      const auto actions = ActionsFor(actionTarget_);
      if (empty) {
        addSection(L"Actions", take(actions));
      } else {
        std::vector<leancast::core::SearchItem> searchItems;
        for (const auto& item : actions) searchItems.push_back(ToSearchItem(item));
        auto order = leancast::core::Search(query_, searchItems);
        std::vector<DisplayItem> hits;
        for (const auto index : order) hits.push_back(actions[index]);
        addSection(L"Actions", take(hits));
      }

      sections_ = std::move(sections);
      flatItems_.clear();
      for (const auto& section : sections_) {
        flatItems_.insert(flatItems_.end(), section.items.begin(), section.items.end());
      }
      if (selected_ >= static_cast<int>(flatItems_.size())) selected_ = std::max<int>(0, static_cast<int>(flatItems_.size()) - 1);
      ApplyWindowSize();
      return;
    }

    if (empty) {
      std::map<std::wstring, DisplayItem> byId;
      for (const auto& item : appItems) {
        for (const auto& key : AppKeys(item.app)) byId[key] = item;
      }
      std::vector<DisplayItem> pinned;
      for (const auto& item : appItems) {
        if (ContainsAnyAppKey(settings_.pinnedApps, item.app)) pinned.push_back(item);
      }
      std::vector<DisplayItem> recent;
      for (const auto& id : settings_.recentApps) {
        if (byId.contains(id)) recent.push_back(byId[id]);
      }
      std::vector<DisplayItem> system;
      for (const auto& item : appItems) {
        if (item.app.systemEssential || item.app.source == L"alias") system.push_back(item);
      }
      addSection(L"Pinned", take(pinned, 12));
      addSection(L"Recently used", take(recent, 8));
      addSection(L"Open windows", take(windowItems));
      addSection(L"System essentials", take(system, 8));
      addSection(L"Commands", take(commandItems, 8));
    } else {
      if (const auto calculation = leancast::calculator::TryEvaluate(query_)) {
        addSection(L"Calculator", take({CalculatorDisplay(*calculation)}, 1));
      }

      std::vector<DisplayItem> pool = windowItems;
      pool.insert(pool.end(), appItems.begin(), appItems.end());
      pool.insert(pool.end(), commandItems.begin(), commandItems.end());

      std::vector<leancast::core::SearchItem> searchItems;
      for (const auto& item : pool) searchItems.push_back(ToSearchItem(item));
      std::set<std::wstring> recentIds(settings_.recentApps.begin(), settings_.recentApps.end());
      auto order = leancast::core::Search(query_, searchItems, recentIds);
      const size_t limit = static_cast<size_t>(std::clamp(settings_.maxResults, MIN_RESULTS, MAX_RESULT_SETTING));
      if (order.size() > limit) order.resize(limit);

      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(pool[index]);

      if (!hits.empty()) addSection(L"Best match", take({hits.front()}, 1));

      std::vector<DisplayItem> rest(hits.size() > 1 ? hits.begin() + 1 : hits.end(), hits.end());
      std::vector<DisplayItem> recent;
      std::vector<DisplayItem> appsOnly;
      std::vector<DisplayItem> open;
      std::vector<DisplayItem> system;
      std::vector<DisplayItem> commands;
      std::vector<DisplayItem> other;
      for (const auto& item : rest) {
        if (item.isCommand) commands.push_back(item);
        if (!item.isWindow && !item.isCommand && recentIds.contains(PrimaryAppId(item.app))) recent.push_back(item);
        if (!item.isWindow && !item.isCommand && item.app.source == L"shortcut") appsOnly.push_back(item);
        if (item.isWindow) open.push_back(item);
        if (!item.isWindow && !item.isCommand && item.app.source != L"shortcut") system.push_back(item);
        other.push_back(item);
      }
      addSection(L"Commands", take(commands, 20));
      addSection(L"Recently used", take(recent, 8));
      addSection(L"Apps", take(appsOnly, 80));
      addSection(L"Open windows", take(open, 40));
      addSection(L"System & Store apps", take(system, 80));
      addSection(L"Other matches", take(other, 40));
    }

    sections_ = std::move(sections);
    flatItems_.clear();
    for (const auto& section : sections_) {
      flatItems_.insert(flatItems_.end(), section.items.begin(), section.items.end());
    }
    if (selected_ >= static_cast<int>(flatItems_.size())) selected_ = std::max<int>(0, static_cast<int>(flatItems_.size()) - 1);
    ApplyWindowSize();
  }

  leancast::core::SearchItem ToSearchItem(const DisplayItem& item) {
    leancast::core::SearchItem out;
    if (item.isCalculator) {
      out.id = item.Key();
      out.kind = L"calculator";
      out.source = L"calculator";
      out.name = item.calculationResult;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isCommand) {
      out.id = item.Key();
      out.kind = L"command";
      out.source = L"command";
      out.name = item.commandName;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isAction) {
      out.id = item.Key();
      out.kind = L"action";
      out.source = L"action";
      out.name = item.commandName;
      out.keywords = item.commandKeywords;
      out.keywords.push_back(item.commandDetail);
      out.systemEssential = true;
    } else if (item.isWindow) {
      out.id = L"win:" + std::to_wstring(reinterpret_cast<uintptr_t>(item.window.hwnd));
      out.kind = L"window";
      out.name = item.window.name;
      out.processName = item.window.processName;
      out.exe = item.window.exe;
    } else {
      out.id = item.app.id;
      out.path = item.app.path;
      out.kind = L"app";
      out.source = item.app.source;
      out.name = item.app.name;
      out.targetPath = item.app.targetPath;
      out.launchTarget = item.app.launchTarget;
      out.keywords = item.app.keywords;
      out.systemEssential = item.app.systemEssential;
      out.pinned = ContainsAnyAppKey(settings_.pinnedApps, item.app);
      for (const auto& key : AppKeys(item.app)) {
        if (auto alias = settings_.appAliases.find(key); alias != settings_.appAliases.end()) {
          out.keywords.push_back(alias->second);
        }
        if (auto usage = settings_.usageStats.find(key); usage != settings_.usageStats.end()) {
          out.usageCount = std::max(out.usageCount, usage->second.launches);
          out.lastUsed = std::max(out.lastUsed, usage->second.lastUsed);
        }
      }
    }
    return out;
  }

  void ShowOverlay(View view) {
    view_ = view;
    HWND foreground = GetForegroundWindow();
    if (!visible_) {
      if (foreground && foreground != hwnd_) lastActiveWindow_ = foreground;
    }
    RefreshWindows();
    actionMode_ = false;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    BuildSections();
    PositionWindow();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    // Auto-focus overlay
    keybd_event(0xE8, 0, 0, 0);
    keybd_event(0xE8, 0, KEYEVENTF_KEYUP, 0);

    DWORD foregroundThreadId = GetWindowThreadProcessId(foreground, nullptr);
    DWORD currentThreadId = GetCurrentThreadId();
    if (foreground && foreground != hwnd_ && foregroundThreadId != currentThreadId) {
      AttachThreadInput(foregroundThreadId, currentThreadId, TRUE);
      SetForegroundWindow(hwnd_);
      SetActiveWindow(hwnd_);
      SetFocus(hwnd_);
      AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    } else {
      SetForegroundWindow(hwnd_);
      SetActiveWindow(hwnd_);
      SetFocus(hwnd_);
    }

    SetTimer(hwnd_, 1, 200, nullptr);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void HideOverlay(bool restoreFocus) {
    KillTimer(hwnd_, 1);
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    if (restoreFocus && lastActiveWindow_) {
      FocusWindow(lastActiveWindow_);
      lastActiveWindow_ = nullptr;
    }
  }

  void ToggleOverlay() {
    if (visible_) HideOverlay(true);
    else ShowOverlay(View::Search);
  }

  void EnterActionMode(const DisplayItem& target) {
    if (target.isCommand || target.isAction) return;
    actionMode_ = true;
    actionTarget_ = target;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    BuildSections();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ExitActionMode() {
    actionMode_ = false;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    BuildSections();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ClearQuery() {
    query_.clear();
    caret_ = 0;
  }

  void ClampCaret() {
    caret_ = std::min(caret_, query_.size());
  }

  void SetCaretFromSearchClick(float x) {
    ClampCaret();
    if (query_.empty() || !dwriteFactory_) {
      caret_ = query_.size();
      return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right - rc.left);
    const float textLeft = 52.0f;
    const float textWidth = std::max(1.0f, width - 94.0f - textLeft);
    if (x <= textLeft) {
      caret_ = 0;
      return;
    }
    if (x >= textLeft + textWidth) {
      caret_ = query_.size();
      return;
    }

    ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = dwriteFactory_->CreateTextLayout(
        query_.c_str(),
        static_cast<UINT32>(query_.size()),
        inputFormat_.Get(),
        textWidth,
        48.0f,
        layout.GetAddressOf());
    if (FAILED(hr)) {
      caret_ = query_.size();
      return;
    }

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (SUCCEEDED(layout->HitTestPoint(x - textLeft, 24.0f, &trailing, &inside, &metrics))) {
      caret_ = static_cast<size_t>(metrics.textPosition + (trailing ? metrics.length : 0));
      ClampCaret();
    }
  }

  void PositionWindow() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const int width = OverlayWidth();
    const int height = CurrentHeight();
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    const int y = mi.rcWork.top + static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.22);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  }

  int ResultsContentHeight() const {
    int height = 0;
    for (const auto& section : sections_) {
      height += 26;
      height += static_cast<int>(section.items.size()) * 48;
    }
    return height;
  }

  int CurrentHeight() const {
    if (view_ == View::Settings) return 820;
    if (!settings_.compactMode) return WIN_HEIGHT;
    if (Trim(query_).empty() && !actionMode_) return COMPACT_BASE_HEIGHT;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const int maxHeight = static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.7);
    return std::clamp(COMPACT_BASE_HEIGHT + ResultsContentHeight(), COMPACT_BASE_HEIGHT, maxHeight);
  }

  void ApplyWindowSize() {
    const int height = CurrentHeight();
    const int width = OverlayWidth();
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    const int currentWidth = rc.right - rc.left;
    const int currentHeight = rc.bottom - rc.top;
    if (currentWidth != width || currentHeight != height) {
      SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, width, height, SWP_NOACTIVATE | SWP_NOMOVE);
      ApplyRoundedRegion(width, height);
    }
  }

  int OverlayWidth() const {
    return std::clamp(settings_.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
  }

  void ApplyRoundedRegion(int width, int height) {
    BOOL compositionEnabled = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&compositionEnabled)) && compositionEnabled) {
      SetWindowRgn(hwnd_, nullptr, TRUE);
      return;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 14, 14);
    SetWindowRgn(hwnd_, region, TRUE);
  }

  COLORREF ActiveAccent() const {
    if (!settings_.syncAccentColor) return ColorRefFromHex(settings_.customAccentColor);
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
      return RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
    }
    return RGB(0x5b, 0x6c, 0xff);
  }

  void Paint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    CreateDeviceResources();
    if (renderTarget_) {
      renderTarget_->BeginDraw();
      renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f));
      hits_.clear();
      if (view_ == View::Settings) DrawSettings();
      else DrawSearch();
      const HRESULT hr = renderTarget_->EndDraw();
      if (hr == D2DERR_RECREATE_TARGET) {
        renderTarget_.Reset();
        iconBitmaps_.clear();
      }
    }
    EndPaint(hwnd_, &ps);
  }

  ComPtr<ID2D1SolidColorBrush> Brush(D2D1_COLOR_F color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    renderTarget_->CreateSolidColorBrush(color, brush.GetAddressOf());
    return brush;
  }

  void FillRound(RectF rect, float radius, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get());
  }

  void StrokeRound(RectF rect, float radius, D2D1_COLOR_F color, float width = 1.0f) {
    auto brush = Brush(color);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get(), width);
  }

  void DrawTextBlock(const std::wstring& text, RectF rect, IDWriteTextFormat* format, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, ToD2D(rect), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  void DrawSearchIcon(float x, float y, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 8, y + 8), 6, 6), brush.Get(), 2.0f);
    renderTarget_->DrawLine(D2D1::Point2F(x + 13, y + 13), D2D1::Point2F(x + 19, y + 19), brush.Get(), 2.0f);
  }

  void DrawGearIcon(float cx, float cy, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.0f, 5.0f), brush.Get(), 2.0f);
    renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.5f, 1.5f), brush.Get(), 1.5f);
    for (int i = 0; i < 8; ++i) {
      float angle = i * 3.14159265f / 4.0f;
      float sinA = std::sin(angle);
      float cosA = std::cos(angle);
      renderTarget_->DrawLine(
        D2D1::Point2F(cx + cosA * 5.0f, cy + sinA * 5.0f),
        D2D1::Point2F(cx + cosA * 8.0f, cy + sinA * 8.0f),
        brush.Get(),
        2.0f
      );
    }
  }

  void DrawSearch() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right - rc.left);
    const float height = static_cast<float>(rc.bottom - rc.top);
    const COLORREF accent = ActiveAccent();
    const COLORREF bg = RGB(0x1c, 0x1c, 0x20);

    FillRound({0, 0, width, height}, 14, D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.94f));
    StrokeRound({0.5f, 0.5f, width - 0.5f, height - 0.5f}, 14, D2D1::ColorF(1, 1, 1, 0.08f));

    DrawSearchIcon(18, 20, D2D1::ColorF(0.60f, 0.60f, 0.64f, 1));
    const std::wstring input = query_.empty()
        ? (actionMode_ ? L"Actions for " + actionTarget_.Name() : L"Search apps, windows, or commands...")
        : query_;
    DrawTextBlock(input, {52, 15, width - 94, 48}, inputFormat_.Get(), query_.empty() ? D2D1::ColorF(0.60f, 0.60f, 0.64f) : D2D1::ColorF(0.95f, 0.95f, 0.96f));

    float caretX = 52.0f;
    if (!query_.empty()) {
      ClampCaret();
      const std::wstring beforeCaret = query_.substr(0, caret_);
      ComPtr<IDWriteTextLayout> layout;
      HRESULT hr = dwriteFactory_->CreateTextLayout(
          beforeCaret.c_str(),
          static_cast<UINT32>(beforeCaret.size()),
          inputFormat_.Get(),
          width - 94 - 52,
          48,
          layout.GetAddressOf()
      );
      if (SUCCEEDED(hr)) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        caretX = 52.0f + metrics.width;
      }
    }
    const bool showCaret = (GetTickCount() / GetCaretBlinkTime()) % 2 == 0;
    if (showCaret && view_ == View::Search) {
      auto caretBrush = Brush(D2DColor(accent));
      renderTarget_->DrawLine(
          D2D1::Point2F(caretX, 20.0f),
          D2D1::Point2F(caretX, 42.0f),
          caretBrush.Get(),
          1.5f
      );
    }

    if (caching_) {
      FillRound({width - 82, 26, width - 74, 34}, 4, D2DColor(accent));
    }
    hits_.push_back({{width - 52, 14, width - 16, 50}, HitType::Gear});
    DrawGearIcon(width - 34, 32, D2D1::ColorF(0.72f, 0.72f, 0.76f));

    if (!settings_.compactMode || !query_.empty()) {
      auto border = Brush(D2D1::ColorF(1, 1, 1, 0.08f));
      renderTarget_->DrawLine(D2D1::Point2F(0, 60), D2D1::Point2F(width, 60), border.Get(), 1);
    }

    if (settings_.compactMode && query_.empty() && !actionMode_) return;

    const bool showFooter = !settings_.compactMode;
    const float footerHeight = showFooter ? 40.0f : 0.0f;
    const float resultsTop = 60.0f;
    const float resultsBottom = height - footerHeight;
    const float viewHeight = resultsBottom - resultsTop;
    const int contentHeight = ResultsContentHeight();
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight - static_cast<int>(viewHeight)));

    float y = resultsTop - static_cast<float>(scroll_);
    int rowIndex = 0;
    renderTarget_->PushAxisAlignedClip(D2D1::RectF(0, resultsTop, width, resultsBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (const auto& section : sections_) {
      if (y + 26 >= resultsTop && y <= resultsBottom) {
        DrawTextBlock(section.title, {12, y + 8, width - 12, y + 24}, sectionFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
      }
      y += 26;
      for (const auto& item : section.items) {
        RectF rowRect{8, y, width - 8, y + 46};
        if (rowRect.bottom >= resultsTop && rowRect.top <= resultsBottom) {
          DrawResultRow(item, rowRect, rowIndex);
        }
        y += 48;
        ++rowIndex;
      }
    }
    renderTarget_->PopAxisAlignedClip();

    if (sections_.empty()) {
      DrawTextBlock(appsReady_ ? L"No results" : L"Loading apps...", {0, 170, width, 210}, centerFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    }

    if (showFooter) {
      auto border = Brush(D2D1::ColorF(1, 1, 1, 0.06f));
      renderTarget_->DrawLine(D2D1::Point2F(0, height - 40.0f), D2D1::Point2F(width, height - 40.0f), border.Get(), 1.0f);
      DrawTextBlock(L"LeanCast", {16, height - 28.0f, 160, height - 12.0f}, footerFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
      DrawTextBlock(L"Up/Down Navigate | Enter Open | Tab Actions | Esc Close", {200, height - 28.0f, width - 16.0f, height - 12.0f}, footerRightFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    }
  }

  void DrawResultRow(const DisplayItem& item, RectF rowRect, int rowIndex) {
    const COLORREF accent = ActiveAccent();
    const bool selected = rowIndex == selected_;
    if (selected) FillRound(rowRect, 8, Mix(accent, RGB(0x1c, 0x1c, 0x20), 0.22f, 1.0f));
    hits_.push_back({rowRect, HitType::Result, rowIndex});

    const float iconX = rowRect.left + 12;
    const float iconY = rowRect.top + 11;
    auto bitmap = IconBitmap(item.IconKey());
    if (bitmap) {
      renderTarget_->DrawBitmap(bitmap.Get(), D2D1::RectF(iconX, iconY, iconX + 24, iconY + 24), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      FillRound({iconX, iconY, iconX + 24, iconY + 24}, 6, D2D1::ColorF(0.23f, 0.23f, 0.28f));
      std::wstring letter = item.Name().empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(std::towupper(item.Name()[0])));
      DrawTextBlock(letter, {iconX, iconY + 1, iconX + 24, iconY + 24}, centerFormat_.Get(), D2D1::ColorF(0.70f, 0.70f, 0.74f));
    }

    DrawTextBlock(item.Name(), {rowRect.left + 50, rowRect.top + 7, rowRect.right - 180, rowRect.top + 27}, rowFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    DrawTextBlock(SourceLabel(item), {rowRect.left + 50, rowRect.top + 28, rowRect.right - 180, rowRect.bottom}, subFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    if (selected) {
      DrawTextBlock(ActionHint(item), {rowRect.right - 170, rowRect.top + 15, rowRect.right - 10, rowRect.bottom}, subFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    }
  }

  std::wstring SourceLabel(const DisplayItem& item) const {
    if (item.isCalculator) return item.commandDetail;
    if (item.isCommand || item.isAction) return item.commandDetail;
    if (item.isWindow) return item.window.processName.empty() ? L"Open window" : L"Open window - " + item.window.processName;
    if (item.app.source == L"shortcut") return L"Desktop app";
    if (item.app.source == L"alias") return L"System app";
    if (item.app.source == L"appx") return L"Store/System app";
    return L"Start menu app";
  }

  std::wstring ActionHint(const DisplayItem& item) const {
    if (item.isCalculator) return L"Enter Copy";
    if (item.isCommand) return L"Enter Run";
    if (item.isAction) return L"Enter Apply";
    if (item.isWindow) return L"Enter Switch";
    return item.app.adminSupported ? L"Enter Open | Tab Actions | Ctrl+Shift Admin" : L"Enter Open | Tab Actions";
  }

  void DrawSettings() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right - rc.left);
    const float height = static_cast<float>(rc.bottom - rc.top);
    const COLORREF accent = ActiveAccent();

    FillRound({0, 0, width, height}, 14, D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.94f));
    StrokeRound({0.5f, 0.5f, width - 0.5f, height - 0.5f}, 14, D2D1::ColorF(1, 1, 1, 0.08f));
    hits_.push_back({{10, 10, 46, 46}, HitType::Back});
    DrawTextBlock(L"<", {10, 11, 46, 45}, centerFormat_.Get(), D2D1::ColorF(0.72f, 0.72f, 0.76f));
    DrawTextBlock(L"Settings", {56, 16, width - 16, 45}, titleFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    auto border = Brush(D2D1::ColorF(1, 1, 1, 0.08f));
    renderTarget_->DrawLine(D2D1::Point2F(0, 56), D2D1::Point2F(width, 56), border.Get(), 1);

    float y = 76;
    DrawTextBlock(L"Global Shortcut", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 25;
    std::wstring current = settings_.shortcut.empty() || settings_.shortcut == L"none" ? L"None" : settings_.shortcut;
    DrawTextBlock(L"Keyboard shortcut to open LeanCast. Current: " + current, {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 32;

    RectF record{20, y, width - 155, y + 42};
    FillRound(record, 8, recording_ ? Mix(accent, RGB(0x1c, 0x1c, 0x20), 0.28f) : D2D1::ColorF(0.09f, 0.09f, 0.11f));
    StrokeRound(record, 8, recording_ ? D2DColor(accent) : D2D1::ColorF(1, 1, 1, 0.18f));
    hits_.push_back({record, HitType::RecordShortcut});
    const std::wstring recordText = recording_ ? L"Press a key..." : (!pendingShortcut_.empty() ? pendingShortcut_ : L"Record new shortcut");
    DrawTextBlock(recordText, {record.left + 12, record.top + 12, record.right - 12, record.bottom}, bodyFormat_.Get(), recording_ ? D2DColor(accent) : D2D1::ColorF(0.95f, 0.95f, 0.96f));

    if (!pendingShortcut_.empty()) {
      RectF save{width - 140, y, width - 20, y + 42};
      FillRound(save, 8, D2DColor(accent));
      hits_.push_back({save, HitType::SaveShortcut});
      DrawTextBlock(L"Save", save, centerFormat_.Get(), D2D1::ColorF(1, 1, 1));
    } else if (!settings_.shortcut.empty() && settings_.shortcut != L"none") {
      RectF clear{width - 140, y, width - 20, y + 42};
      StrokeRound(clear, 8, D2D1::ColorF(1.0f, 0.36f, 0.36f, 0.6f));
      hits_.push_back({clear, HitType::ClearShortcut});
      DrawTextBlock(L"Clear", clear, centerFormat_.Get(), D2D1::ColorF(1.0f, 0.36f, 0.36f));
    }
    y += 62;
    DrawTextBlock(L"At least one modifier is required. Examples: Alt+Space, Control+Space.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));

    y += 40;
    DrawTextBlock(L"Start on Startup", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 22;
    DrawTextBlock(L"Launch LeanCast automatically when you log into Windows.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 28;
    DrawToggle({20, y, 120, y + 34}, settings_.startOnStartup, settings_.startOnStartup ? L"On" : L"Off", HitType::StartupToggle);

    y += 48;
    DrawTextBlock(L"Compact Mode", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 22;
    DrawTextBlock(L"Shows only the search bar at rest; results expand below.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 28;
    DrawToggle({20, y, 120, y + 34}, settings_.compactMode, settings_.compactMode ? L"On" : L"Off", HitType::CompactToggle);

    y += 48;
    DrawTextBlock(L"Open Window Results", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 22;
    DrawTextBlock(L"Include currently open windows in search results.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 28;
    DrawToggle({20, y, 120, y + 34}, settings_.showOpenWindows, settings_.showOpenWindows ? L"On" : L"Off", HitType::ShowWindowsToggle);

    y += 48;
    DrawTextBlock(L"Store/System Apps", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 22;
    DrawTextBlock(L"Include AppsFolder, Store, and system alias entries.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 28;
    DrawToggle({20, y, 120, y + 34}, settings_.showStoreApps, settings_.showStoreApps ? L"On" : L"Off", HitType::ShowStoreAppsToggle);

    y += 48;
    DrawTextBlock(L"Overlay Width", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    DrawTextBlock(std::to_wstring(OverlayWidth()) + L" px", {180, y, 280, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    DrawSettingsButton({width - 130, y - 6, width - 80, y + 28}, L"-", HitType::OverlayWidthDown);
    DrawSettingsButton({width - 70, y - 6, width - 20, y + 28}, L"+", HitType::OverlayWidthUp);

    y += 46;
    DrawTextBlock(L"Max Results", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    DrawTextBlock(std::to_wstring(std::clamp(settings_.maxResults, MIN_RESULTS, MAX_RESULT_SETTING)), {180, y, 280, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    DrawSettingsButton({width - 130, y - 6, width - 80, y + 28}, L"-", HitType::MaxResultsDown);
    DrawSettingsButton({width - 70, y - 6, width - 20, y + 28}, L"+", HitType::MaxResultsUp);

    y += 46;
    DrawTextBlock(L"Maintenance", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 28;
    DrawSettingsButton({20, y, 160, y + 34}, L"Clear Recents", HitType::ClearRecents);
    DrawSettingsButton({176, y, 336, y + 34}, L"Clear Icon Cache", HitType::ClearIconCache);

    y += 52;
    DrawTextBlock(L"Accent Color", {20, y, width - 20, y + 20}, labelFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    y += 22;
    DrawTextBlock(L"Choose between auto-syncing with Windows or selecting a custom color.", {20, y, width - 20, y + 20}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
    y += 28;
    DrawToggle({20, y, 210, y + 34}, settings_.syncAccentColor, settings_.syncAccentColor ? L"Sync with Windows" : L"Custom Color", HitType::AccentToggle);
    if (!settings_.syncAccentColor) {
      RectF colorBox{230, y, 320, y + 34};
      FillRound(colorBox, 8, D2D1::ColorF(0.09f, 0.09f, 0.11f));
      StrokeRound(colorBox, 8, D2D1::ColorF(1, 1, 1, 0.08f));
      FillRound({240, y + 7, 260, y + 27}, 4, D2DColor(ColorRefFromHex(settings_.customAccentColor)));
      DrawTextBlock(settings_.customAccentColor, {268, y + 8, 318, y + 28}, bodyFormat_.Get(), D2D1::ColorF(0.60f, 0.60f, 0.64f));
      hits_.push_back({colorBox, HitType::AccentColor});
    }
  }

  void DrawToggle(RectF rect, bool on, const std::wstring& text, HitType type) {
    const COLORREF accent = ActiveAccent();
    FillRound(rect, 17, D2D1::ColorF(0.09f, 0.09f, 0.11f));
    StrokeRound(rect, 17, D2D1::ColorF(1, 1, 1, 0.08f));
    RectF track{rect.left + 7, rect.top + 7, rect.left + 43, rect.top + 27};
    FillRound(track, 10, on ? D2DColor(accent) : D2D1::ColorF(1, 1, 1, 0.15f));
    const float knobX = on ? track.right - 18 : track.left + 2;
    FillRound({knobX, track.top + 2, knobX + 16, track.top + 18}, 8, D2D1::ColorF(1, 1, 1));
    DrawTextBlock(text, {rect.left + 54, rect.top + 8, rect.right - 10, rect.bottom}, bodyFormat_.Get(), on ? D2D1::ColorF(0.95f, 0.95f, 0.96f) : D2D1::ColorF(0.60f, 0.60f, 0.64f));
    hits_.push_back({rect, type});
  }

  void DrawSettingsButton(RectF rect, const std::wstring& text, HitType type) {
    FillRound(rect, 8, D2D1::ColorF(0.09f, 0.09f, 0.11f));
    StrokeRound(rect, 8, D2D1::ColorF(1, 1, 1, 0.16f));
    DrawTextBlock(text, rect, centerFormat_.Get(), D2D1::ColorF(0.95f, 0.95f, 0.96f));
    hits_.push_back({rect, type});
  }

  ComPtr<ID2D1Bitmap> IconBitmap(const std::wstring& key) {
    if (key.empty() || !renderTarget_) return nullptr;
    if (iconBitmaps_.contains(key)) return iconBitmaps_[key];

    const auto png = IconCachePath(key);
    std::error_code ec;
    if (std::filesystem::exists(png, ec)) {
      auto bitmap = LoadBitmapFromFile(png);
      if (bitmap) {
        iconBitmaps_[key] = bitmap;
        return bitmap;
      }
    }

    QueueIcon(key);
    return nullptr;
  }

  void QueueIcon(const std::wstring& key) {
    {
      std::lock_guard lock(iconQueueMutex_);
      if (pendingIcons_.contains(key)) return;
      pendingIcons_.insert(key);
    }

    std::lock_guard threadLock(workerThreadsMutex_);
    iconThreads_.emplace_back([this, key](std::stop_token stopToken) {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (!stopToken.stop_requested() && !stopThreads_) ResolveIconToCache(key);
      {
        std::lock_guard lock(iconQueueMutex_);
        pendingIcons_.erase(key);
      }
      if (!stopToken.stop_requested() && !stopThreads_) PostMessageW(hwnd_, WM_ICON_READY, 0, 0);
      CoUninitialize();
    });
  }

  ComPtr<ID2D1Bitmap> LoadBitmapFromFile(const std::filesystem::path& path) {
    if (!wicFactory_ || !renderTarget_) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf()))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf());
    return bitmap;
  }

  bool ResolveIconToCache(const std::wstring& key) {
    const auto png = IconCachePath(key);
    std::error_code ec;
    if (std::filesystem::exists(png, ec)) return true;
    HBITMAP bitmap = CreateShellBitmap(key);
    if (!bitmap) return false;
    const bool ok = SaveHBitmapPng(bitmap, png);
    DeleteObject(bitmap);
    return ok;
  }

  HBITMAP CreateShellBitmap(const std::wstring& key) {
    std::wstring parseName = key;
    if (StartsWith(key, L"appsFolder:")) {
      parseName = L"shell:AppsFolder\\" + key.substr(11);
    }

    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(SHCreateItemFromParsingName(parseName.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
      HBITMAP bitmap = nullptr;
      SIZE size{64, 64};
      if (SUCCEEDED(factory->GetImage(size, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap)) && bitmap) return bitmap;
    }

    if (Lower(key).ends_with(L".lnk")) {
      ShortcutInfo info;
      if (LoadShortcut(key, info)) {
        if (!info.iconPath.empty()) {
          if (HBITMAP bitmap = BitmapFromPathIcon(info.iconPath, info.iconIndex)) return bitmap;
        }
        if (!info.target.empty()) {
          if (HBITMAP bitmap = CreateShellBitmap(info.target)) return bitmap;
        }
      }
    }
    return BitmapFromPathIcon(key, 0);
  }

  HBITMAP BitmapFromPathIcon(const std::wstring& path, int index) {
    HICON icon = nullptr;
    if (!path.empty()) {
      ExtractIconExW(path.c_str(), index, &icon, nullptr, 1);
    }
    if (!icon) {
      SHFILEINFOW info{};
      if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) icon = info.hIcon;
    }
    if (!icon) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(dc, bitmap);
    DrawIconEx(dc, 0, 0, icon, 64, 64, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    DestroyIcon(icon);
    return bitmap;
  }

  bool SaveHBitmapPng(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!wicFactory_ || !bitmap) return false;
    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, wicBitmap.GetAddressOf()))) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), bag.GetAddressOf()))) return false;
    if (FAILED(frame->Initialize(bag.Get()))) return false;

    UINT w = 0;
    UINT h = 0;
    wicBitmap->GetSize(&w, &h);
    frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&format);
    if (FAILED(frame->WriteSource(wicBitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
  }

  void OnKeyDown(UINT vk) {
    if (vk == VK_ESCAPE) {
      if (view_ == View::Search && actionMode_) {
        ExitActionMode();
      } else if (view_ == View::Settings) {
        recording_ = false;
        view_ = View::Search;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        HideOverlay(true);
      }
      return;
    }

    if (view_ != View::Search) return;

    if (vk == VK_DOWN) {
      if (!flatItems_.empty()) selected_ = std::min<int>(selected_ + 1, static_cast<int>(flatItems_.size()) - 1);
      EnsureSelectedVisible();
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_UP) {
      selected_ = std::max(0, selected_ - 1);
      EnsureSelectedVisible();
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_HOME) {
      if (!query_.empty()) {
        caret_ = 0;
      } else {
        selected_ = 0;
        EnsureSelectedVisible();
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_END) {
      if (!query_.empty()) {
        caret_ = query_.size();
      } else {
        if (!flatItems_.empty()) selected_ = static_cast<int>(flatItems_.size()) - 1;
        EnsureSelectedVisible();
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_PRIOR) {
      selected_ = std::max(0, selected_ - 8);
      EnsureSelectedVisible();
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_NEXT) {
      if (!flatItems_.empty()) selected_ = std::min<int>(selected_ + 8, static_cast<int>(flatItems_.size()) - 1);
      EnsureSelectedVisible();
      InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (vk == VK_RIGHT) {
      if (caret_ < query_.size()) {
        ++caret_;
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (query_.empty() && selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
        EnterActionMode(flatItems_[selected_]);
      }
    } else if (vk == VK_TAB) {
      if (selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) EnterActionMode(flatItems_[selected_]);
    } else if (vk == VK_LEFT) {
      if (caret_ > 0) {
        --caret_;
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (actionMode_) {
        ExitActionMode();
      }
    } else if (vk == VK_RETURN) {
      if (selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
        const bool admin = ModifierPressed(VK_CONTROL) && ModifierPressed(VK_SHIFT);
        Activate(flatItems_[selected_], admin);
      }
    } else if (vk == VK_BACK) {
      if (!query_.empty() && caret_ > 0) {
        if (ModifierPressed(VK_CONTROL)) {
          while (caret_ > 0 && std::iswspace(query_[caret_ - 1])) query_.erase(--caret_, 1);
          while (caret_ > 0 && !std::iswspace(query_[caret_ - 1])) query_.erase(--caret_, 1);
        } else {
          query_.erase(--caret_, 1);
        }
        selected_ = 0;
        scroll_ = 0;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    } else if (vk == VK_DELETE) {
      if (!query_.empty() && caret_ < query_.size()) {
        if (ModifierPressed(VK_CONTROL)) {
          while (caret_ < query_.size() && std::iswspace(query_[caret_])) query_.erase(caret_, 1);
          while (caret_ < query_.size() && !std::iswspace(query_[caret_])) query_.erase(caret_, 1);
        } else {
          query_.erase(caret_, 1);
        }
        selected_ = 0;
        scroll_ = 0;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    }
  }

  void OnChar(wchar_t ch) {
    if (view_ != View::Search) return;
    if (ch >= 32 && ch != 127) {
      ClampCaret();
      query_.insert(query_.begin() + static_cast<std::ptrdiff_t>(caret_), ch);
      ++caret_;
      selected_ = 0;
      scroll_ = 0;
      BuildSections();
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  void EnsureSelectedVisible() {
    int y = 0;
    int row = 0;
    for (const auto& section : sections_) {
      y += 26;
      for (size_t i = 0; i < section.items.size(); ++i) {
        if (row == selected_) {
          const int rowTop = y;
          const int rowBottom = y + 48;
          RECT rc{};
          GetClientRect(hwnd_, &rc);
          const int visible = (rc.bottom - rc.top) - 60 - (settings_.compactMode ? 0 : 36);
          if (rowTop - scroll_ < 0) scroll_ = rowTop;
          else if (rowBottom - scroll_ > visible) scroll_ = rowBottom - visible;
          scroll_ = std::max(0, scroll_);
          return;
        }
        y += 48;
        ++row;
      }
    }
  }

  void OnMouseMove(float x, float y) {
    if (view_ != View::Search) return;
    for (const auto& hit : hits_) {
      if (hit.type == HitType::Result && PointInRect(hit.rect, x, y) && hit.index != selected_) {
        selected_ = hit.index;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }
    }
  }

  void OnClick(float x, float y) {
    if (view_ == View::Search) {
      RECT rc{};
      GetClientRect(hwnd_, &rc);
      const float width = static_cast<float>(rc.right - rc.left);
      if (PointInRect({0, 0, width - 60.0f, 60.0f}, x, y)) {
        SetCaretFromSearchClick(x);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }
    }

    for (const auto& hit : hits_) {
      if (!PointInRect(hit.rect, x, y)) continue;
      switch (hit.type) {
        case HitType::Result:
          if (hit.index >= 0 && hit.index < static_cast<int>(flatItems_.size())) Activate(flatItems_[hit.index], false);
          return;
        case HitType::Gear:
          view_ = View::Settings;
          recording_ = false;
          pendingShortcut_.clear();
          ApplyWindowSize();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::Back:
          view_ = View::Search;
          recording_ = false;
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::RecordShortcut:
          pendingShortcut_.clear();
          recording_ = true;
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::SaveShortcut:
          if (!pendingShortcut_.empty()) {
            settings_.shortcut = pendingShortcut_;
            shortcut_ = ParseShortcut(settings_.shortcut);
            pendingShortcut_.clear();
            SaveSettings(settings_);
            InvalidateRect(hwnd_, nullptr, FALSE);
          }
          return;
        case HitType::ClearShortcut:
          settings_.shortcut = L"none";
          shortcut_ = ParseShortcut(settings_.shortcut);
          pendingShortcut_.clear();
          SaveSettings(settings_);
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::StartupToggle:
          settings_.startOnStartup = !settings_.startOnStartup;
          SetStartOnStartup(settings_.startOnStartup);
          SaveSettings(settings_);
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::ShowWindowsToggle:
          settings_.showOpenWindows = !settings_.showOpenWindows;
          SaveSettings(settings_);
          RefreshWindows();
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::ShowStoreAppsToggle:
          settings_.showStoreApps = !settings_.showStoreApps;
          SaveSettings(settings_);
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::ClearRecents:
          settings_.recentApps.clear();
          settings_.usageStats.clear();
          SaveSettings(settings_);
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::ClearIconCache:
          ClearIconCache();
          return;
        case HitType::OverlayWidthDown:
          settings_.overlayWidth = std::clamp(OverlayWidth() - 40, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
          SaveSettings(settings_);
          PositionWindow();
          ApplyWindowSize();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::OverlayWidthUp:
          settings_.overlayWidth = std::clamp(OverlayWidth() + 40, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
          SaveSettings(settings_);
          PositionWindow();
          ApplyWindowSize();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::MaxResultsDown:
          settings_.maxResults = std::clamp(settings_.maxResults - 25, MIN_RESULTS, MAX_RESULT_SETTING);
          SaveSettings(settings_);
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::MaxResultsUp:
          settings_.maxResults = std::clamp(settings_.maxResults + 25, MIN_RESULTS, MAX_RESULT_SETTING);
          SaveSettings(settings_);
          BuildSections();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::CompactToggle:
          settings_.compactMode = !settings_.compactMode;
          SaveSettings(settings_);
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::AccentToggle:
          settings_.syncAccentColor = !settings_.syncAccentColor;
          SaveSettings(settings_);
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        case HitType::AccentColor:
          ChooseAccentColor();
          return;
      }
    }
  }

  void OnMouseWheel(int delta) {
    if (view_ != View::Search || sections_.empty()) return;
    scroll_ -= delta / WHEEL_DELTA * 72;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int visible = (rc.bottom - rc.top) - 60 - (settings_.compactMode ? 0 : 36);
    scroll_ = std::clamp(scroll_, 0, std::max(0, ResultsContentHeight() - visible));
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ChooseAccentColor() {
    COLORREF custom[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hwnd_;
    cc.rgbResult = ColorRefFromHex(settings_.customAccentColor);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    suppressHide_ = true;
    if (ChooseColorW(&cc)) {
      settings_.customAccentColor = HexFromColorRef(cc.rgbResult);
      settings_.syncAccentColor = false;
      SaveSettings(settings_);
    }
    suppressHide_ = false;
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void Activate(const DisplayItem& item, bool asAdmin) {
    if (item.isCalculator) {
      CopyTextToClipboard(item.calculationResult);
      HideOverlay(false);
      return;
    }

    if (item.isCommand) {
      ExecuteCommand(item.command);
      return;
    }

    if (item.isAction) {
      ExecuteAction(item);
      return;
    }

    if (item.isWindow) {
      HideOverlay(false);
      FocusWindow(item.window.hwnd);
      return;
    }

    HideOverlay(false);
    const bool ok = LaunchApp(item.app, asAdmin && item.app.adminSupported);
    if (ok) TrackRecent(PrimaryAppId(item.app));
  }

  void ExecuteCommand(CommandKind command) {
    switch (command) {
      case CommandKind::Settings:
        actionMode_ = false;
        view_ = View::Settings;
        ClearQuery();
        selected_ = 0;
        scroll_ = 0;
        ApplyWindowSize();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::Quit:
        DestroyWindow(hwnd_);
        return;
      case CommandKind::Restart:
        RestartApp();
        return;
      case CommandKind::RefreshApps:
        StartAppDiscovery();
        ClearQuery();
        selected_ = 0;
        scroll_ = 0;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::ClearIconCache:
        ClearIconCache();
        return;
      case CommandKind::ClearRecents:
        settings_.recentApps.clear();
        settings_.usageStats.clear();
        SaveSettings(settings_);
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::OpenDataFolder:
        ShellExecuteW(nullptr, L"open", UserDataPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        HideOverlay(false);
        return;
    }
  }

  void ExecuteAction(const DisplayItem& item) {
    if (item.actionTargetIsWindow) {
      HWND target = item.actionWindow.hwnd;
      if (!target || !IsWindow(target)) {
        actionMode_ = false;
        RefreshWindows();
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }

      switch (item.action) {
        case ActionKind::Switch:
          HideOverlay(false);
          FocusWindow(target);
          return;
        case ActionKind::Minimize:
          ShowWindowAsync(target, SW_MINIMIZE);
          HideOverlay(false);
          return;
        case ActionKind::MaximizeRestore:
          if (IsIconic(target) || IsZoomed(target)) ShowWindowAsync(target, SW_RESTORE);
          else ShowWindowAsync(target, SW_MAXIMIZE);
          HideOverlay(false);
          return;
        case ActionKind::CloseWindow:
          PostMessageW(target, WM_CLOSE, 0, 0);
          HideOverlay(false);
          return;
        default:
          return;
      }
    }

    const AppEntry& app = item.actionApp;
    const std::wstring id = PrimaryAppId(app);
    switch (item.action) {
      case ActionKind::Open:
      case ActionKind::RunAsAdmin: {
        HideOverlay(false);
        const bool ok = LaunchApp(app, item.action == ActionKind::RunAsAdmin && app.adminSupported);
        if (ok) TrackRecent(id);
        return;
      }
      case ActionKind::OpenLocation:
        RevealAppLocation(app);
        HideOverlay(false);
        return;
      case ActionKind::CopyPath:
        CopyTextToClipboard(AppPathForActions(app));
        HideOverlay(false);
        return;
      case ActionKind::Pin:
        if (!id.empty() && !ContainsValue(settings_.pinnedApps, id)) settings_.pinnedApps.insert(settings_.pinnedApps.begin(), id);
        SaveSettings(settings_);
        actionMode_ = false;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Unpin:
        for (const auto& key : AppKeys(app)) RemoveValue(settings_.pinnedApps, key);
        SaveSettings(settings_);
        actionMode_ = false;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Hide:
        if (!id.empty() && !ContainsValue(settings_.hiddenApps, id)) settings_.hiddenApps.push_back(id);
        SaveSettings(settings_);
        actionMode_ = false;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Unhide:
        for (const auto& key : AppKeys(app)) RemoveValue(settings_.hiddenApps, key);
        SaveSettings(settings_);
        actionMode_ = false;
        BuildSections();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      default:
        return;
    }
  }

  std::wstring AppPathForActions(const AppEntry& app) const {
    if (!app.path.empty()) return app.path;
    if (!app.targetPath.empty()) return app.targetPath;
    return app.launchTarget;
  }

  void RevealAppLocation(const AppEntry& app) const {
    const std::wstring path = AppPathForActions(app);
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  }

  void CopyTextToClipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
      void* buffer = GlobalLock(memory);
      if (buffer) {
        memcpy(buffer, text.c_str(), bytes);
        GlobalUnlock(memory);
        SetClipboardData(CF_UNICODETEXT, memory);
        memory = nullptr;
      }
      if (memory) GlobalFree(memory);
    }
    CloseClipboard();
  }

  void ClearIconCache() {
    StopIconThreads();
    {
      std::lock_guard lock(iconQueueMutex_);
      pendingIcons_.clear();
    }
    iconBitmaps_.clear();
    std::error_code ec;
    std::filesystem::remove_all(IconCacheDir(), ec);
    std::filesystem::create_directories(IconCacheDir(), ec);
    StartAppDiscovery();
    BuildSections();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void StopIconThreads() {
    std::lock_guard lock(workerThreadsMutex_);
    for (auto& thread : iconThreads_) {
      if (thread.joinable()) thread.request_stop();
    }
    for (auto& thread : iconThreads_) {
      if (thread.joinable()) thread.join();
    }
    iconThreads_.clear();
  }

  void RestartApp() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring command = L"/c timeout /t 1 /nobreak >nul & start \"\" \"" + std::wstring(exePath) + L"\" --show";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", command.c_str(), nullptr, SW_HIDE);
    DestroyWindow(hwnd_);
  }

  bool LaunchApp(const AppEntry& app, bool asAdmin) {
    if (app.launchType == LaunchType::AppsFolder) {
      const std::wstring args = L"shell:AppsFolder\\" + app.launchTarget;
      HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      return reinterpret_cast<intptr_t>(result) > 32;
    }

    std::wstring file = app.launchTarget;
    std::wstring args = app.args;
    std::wstring cwd = app.cwd;

    if (asAdmin && app.launchType == LaunchType::Shortcut) {
      ShortcutInfo info;
      if (LoadShortcut(app.launchTarget, info) && !info.target.empty()) {
        file = info.target;
        args = info.args;
        cwd = info.cwd;
      }
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = asAdmin ? L"runas" : L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) == TRUE;
  }

  void TrackRecent(const std::wstring& id) {
    if (id.empty()) return;
    std::vector<std::wstring> next{id};
    for (const auto& existing : settings_.recentApps) {
      if (existing != id) next.push_back(existing);
      if (next.size() >= RECENT_LIMIT) break;
    }
    settings_.recentApps = std::move(next);
    auto& usage = settings_.usageStats[id];
    usage.launches = std::min(usage.launches + 1, 1000000);
    usage.lastUsed = UnixNow();
    SaveSettings(settings_);
  }

  void OnTray(LPARAM lParam) {
    const UINT event = LOWORD(lParam);
    if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
      ShowOverlay(View::Search);
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
      POINT pt{};
      GetCursorPos(&pt);
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1, L"Open LeanCast");
      AppendMenuW(menu, MF_STRING, 2, L"Settings");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, 3, L"Quit");
      SetForegroundWindow(hwnd_);
      const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
      DestroyMenu(menu);
      if (cmd == 1) ShowOverlay(View::Search);
      else if (cmd == 2) ShowOverlay(View::Settings);
      else if (cmd == 3) DestroyWindow(hwnd_);
    }
  }

  HINSTANCE instance_ = nullptr;
  std::wstring cmdLine_;
  HWND hwnd_ = nullptr;
  NOTIFYICONDATAW tray_{};
  HHOOK hook_ = nullptr;
  Settings settings_;
  ShortcutSpec shortcut_;
  HWND lastActiveWindow_ = nullptr;
  bool visible_ = false;
  bool suppressHide_ = false;
  View view_ = View::Search;
  std::wstring query_;
  size_t caret_ = 0;
  int selected_ = 0;
  int scroll_ = 0;
  bool actionMode_ = false;
  DisplayItem actionTarget_;
  bool recording_ = false;
  std::wstring pendingShortcut_;
  UINT lastModifierPressed_ = 0;
  bool otherKeyWhileRecordingModifier_ = false;
  ShortcutRuntime shortcutRuntime_;
  std::atomic<bool> stopThreads_ = false;
  std::atomic<bool> caching_ = false;
  bool appsReady_ = false;
  std::jthread discoveryThread_;
  std::mutex workerThreadsMutex_;
  std::vector<std::jthread> iconThreads_;
  std::mutex dataMutex_;
  std::vector<AppEntry> apps_;
  std::vector<WindowEntry> windows_;
  std::vector<Section> sections_;
  std::vector<DisplayItem> flatItems_;
  std::vector<HitTarget> hits_;
  std::mutex iconQueueMutex_;
  std::set<std::wstring> pendingIcons_;
  std::map<std::wstring, ComPtr<ID2D1Bitmap>> iconBitmaps_;

  ComPtr<ID2D1Factory> d2dFactory_;
  ComPtr<IDWriteFactory> dwriteFactory_;
  ComPtr<IWICImagingFactory> wicFactory_;
  ComPtr<ID2D1HwndRenderTarget> renderTarget_;
  ComPtr<IDWriteTextFormat> inputFormat_;
  ComPtr<IDWriteTextFormat> rowFormat_;
  ComPtr<IDWriteTextFormat> subFormat_;
  ComPtr<IDWriteTextFormat> sectionFormat_;
  ComPtr<IDWriteTextFormat> footerFormat_;
  ComPtr<IDWriteTextFormat> footerRightFormat_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> labelFormat_;
  ComPtr<IDWriteTextFormat> bodyFormat_;
  ComPtr<IDWriteTextFormat> buttonFormat_;
  ComPtr<IDWriteTextFormat> centerFormat_;
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(kWindowClass, L"LeanCast");
    if (existing) PostMessageW(existing, WM_SHOW_SEARCH, 0, 0);
    CloseHandle(mutex);
    return 0;
  }

  std::wstring cmdLineStr = cmdLine ? cmdLine : L"";
  LeanCastApp app(instance, cmdLineStr);
  const int result = app.Run();
  if (mutex) CloseHandle(mutex);
  return result;
}
