#include "calculator.hpp"
#include "converter.hpp"
#include "core.hpp"
#include "emoji.hpp"
#include "extension_protocol.hpp"
#include "run_command.hpp"
#include "shortcut.hpp"
#include "snippets.hpp"
#include "storage.hpp"
#include "symbols.hpp"
#include "theme.hpp"
#include "updater.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using leancast::core::ScoreItem;
using leancast::core::ScoreText;
using leancast::core::Search;
using leancast::core::SearchItem;
using leancast::calculator::TryEvaluate;
using leancast::converter::TryConvert;
using leancast::extensions::DiscoverManifests;
using leancast::extensions::HostActionType;
using leancast::extensions::LoadManifest;
using leancast::extensions::ParseActivationResponse;
using leancast::extensions::ParseManifestJson;
using leancast::extensions::ParseQueryResponse;
using leancast::extensions::ResponseSizeAllowed;
using leancast::snippets::ParseSnippetsJson;
using leancast::shortcut::ParseShortcut;
using leancast::shortcut::PressedModifiers;
using leancast::shortcut::ShortcutRecorder;
using leancast::shortcut::ShortcutRuntime;
using leancast::shortcut::ShouldHandleInLowLevelHook;
using leancast::shortcut::ToHotKeySpec;
using leancast::updater::CompareVersionStrings;
using leancast::updater::ExtractSha256Hex;
using leancast::updater::IsEligibleRelease;
using leancast::updater::IsNewerVersion;
using leancast::updater::ParseGitHubReleaseJson;
using leancast::updater::SelectInstallerAsset;
using leancast::updater::SelectSha256Asset;
using leancast::updater::VerifyFileSha256;

namespace {

PressedModifiers Mods(bool ctrl = false, bool alt = false, bool shift = false, bool win = false) {
  return {ctrl, alt, shift, win};
}

void AssertPassOnly(const leancast::shortcut::HookResult& result) {
  assert(!result.consume);
  assert(!result.toggle);
  assert(!result.suppressWinStart);
}

void AssertRecordingPending(const leancast::shortcut::RecordingResult& result) {
  assert(result.consume);
  assert(!result.done);
  assert(!result.canceled);
  assert(result.shortcut.empty());
}

void AssertRecordingCanceled(const leancast::shortcut::RecordingResult& result) {
  assert(result.consume);
  assert(!result.done);
  assert(result.canceled);
  assert(result.shortcut.empty());
}

void AssertRecorded(const leancast::shortcut::RecordingResult& result, const std::wstring& shortcut) {
  assert(result.consume);
  assert(result.done);
  assert(!result.canceled);
  assert(result.shortcut == shortcut);
}

void WriteUtf8(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
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

  {
    // Unit conversions (offline).
    const auto length = TryConvert(L"10 km to mi");
    assert(length && std::fabs(length->value - 6.21371) < 0.001);

    const auto attached = TryConvert(L"10km to mi");
    assert(attached && std::fabs(attached->value - 6.21371) < 0.001);

    const auto mass = TryConvert(L"1 kg in lb");
    assert(mass && std::fabs(mass->value - 2.20462) < 0.001);

    const auto boiling = TryConvert(L"100 c to f");
    assert(boiling && std::fabs(boiling->value - 212.0) < 0.001);

    const auto kelvin = TryConvert(L"0 c to k");
    assert(kelvin && std::fabs(kelvin->value - 273.15) < 0.001);

    const auto arrow = TryConvert(L"5 km -> m");
    assert(arrow && std::fabs(arrow->value - 5000.0) < 0.001);

    // Cross-category and non-conversions are rejected so the calculator wins.
    assert(!TryConvert(L"10 km to kg"));
    assert(!TryConvert(L"2+3"));
    assert(!TryConvert(L"notepad"));

    // Currency conversion uses an injected rate table (units per 1 USD).
    const std::map<std::wstring, double> rates = {{L"USD", 1.0}, {L"EUR", 0.5}, {L"GBP", 0.8}};
    const auto usdEur = TryConvert(L"100 usd to eur", rates);
    assert(usdEur && std::fabs(usdEur->value - 50.0) < 0.001);

    const auto eurGbp = TryConvert(L"10 eur to gbp", rates);
    assert(eurGbp && std::fabs(eurGbp->value - 16.0) < 0.001);

    // An unknown code paired with a known one is not a unit conversion.
    assert(!TryConvert(L"100 abc to eur", rates));
    // With no rate table, currency codes do not resolve.
    assert(!TryConvert(L"100 usd to eur"));

    // Currency-first amount and "=" connector ("USD 5 = GBP").
    const auto usdGbp = TryConvert(L"USD 5 = GBP", rates);
    assert(usdGbp && std::fabs(usdGbp->value - 4.0) < 0.001);

    // A lone currency converts to the supplied locale currency.
    const auto defaultTarget = TryConvert(L"5 usd", rates, L"EUR");
    assert(defaultTarget && std::fabs(defaultTarget->value - 2.5) < 0.001);

    // Currency-first lone amount also uses the locale currency.
    const auto currencyFirstDefault = TryConvert(L"USD 5", rates, L"EUR");
    assert(currencyFirstDefault && std::fabs(currencyFirstDefault->value - 2.5) < 0.001);

    // Without a locale currency a lone amount is not a conversion.
    assert(!TryConvert(L"5 usd", rates));
    // Same source and locale currency is suppressed (no "5 EUR = 5 EUR").
    assert(!TryConvert(L"5 eur", rates, L"EUR"));

    // Currency symbols expand to their ISO codes ("$5", "EUR5 = $").
    const auto symbolGbp = TryConvert(L"$5 = \u00A3", rates);
    assert(symbolGbp && std::fabs(symbolGbp->value - 4.0) < 0.001);

    const auto symbolDefault = TryConvert(L"$5", rates, L"EUR");
    assert(symbolDefault && std::fabs(symbolDefault->value - 2.5) < 0.001);

    const auto euroSymbol = TryConvert(L"\u20AC5 = $", rates);
    assert(euroSymbol && std::fabs(euroSymbol->value - 10.0) < 0.001);
  }

  {
    assert(CompareVersionStrings(L"0.2.0", L"0.2.1") < 0);
    assert(CompareVersionStrings(L"v1.10.0", L"1.2.9") > 0);
    assert(CompareVersionStrings(L"1.0.0", L"1.0.0") == 0);
    assert(IsNewerVersion(L"0.2.0", L"v0.3.0"));
    assert(!IsNewerVersion(L"0.3.0", L"v0.2.9"));

    const auto release = ParseGitHubReleaseJson(
        "{\"tag_name\":\"v0.3.0\",\"name\":\"LeanCast 0.3.0\",\"html_url\":\"https://github.com/GenericLeon0/LeanCast/releases/tag/v0.3.0\","
        "\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"LeanCast-0.3.0-win64.exe\",\"browser_download_url\":\"https://example.test/LeanCast-0.3.0-win64.exe\"},"
        "{\"name\":\"LeanCast-0.3.0-win64.exe.sha256\",\"browser_download_url\":\"https://example.test/LeanCast-0.3.0-win64.exe.sha256\"}"
        "]}");
    assert(release);
    assert(IsEligibleRelease(*release, L"0.2.0"));
    const auto installer = SelectInstallerAsset(*release);
    assert(installer && installer->name == L"LeanCast-0.3.0-win64.exe");
    const auto hash = SelectSha256Asset(*release, *installer);
    assert(hash && hash->name == L"LeanCast-0.3.0-win64.exe.sha256");

    const auto prerelease = ParseGitHubReleaseJson(
        "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":true,\"assets\":[]}");
    assert(prerelease && !IsEligibleRelease(*prerelease, L"0.2.0"));

    const auto extracted = ExtractSha256Hex(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  file.txt");
    assert(extracted && *extracted == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto tempRoot = std::filesystem::temp_directory_path() / L"LeanCastUpdaterCoreTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    WriteUtf8(tempRoot / L"hash.txt", "abc");
    assert(VerifyFileSha256(tempRoot / L"hash.txt",
                            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(!VerifyFileSha256(tempRoot / L"hash.txt",
                             "0000000000000000000000000000000000000000000000000000000000000000"));
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"LeanCastExtensionCoreTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    const auto pluginDir = tempRoot / L"data" / L"plugins" / L"sample";
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"sample\",\"name\":\"Sample\",\"version\":\"1.0\",\"dll\":\"sample.dll\"}");
    const auto manifest = LoadManifest(pluginDir / L"plugin.json");
    assert(manifest.manifest);
    assert(manifest.manifest->id == L"sample");
    assert(manifest.manifest->name == L"Sample");
    assert(manifest.manifest->enabled);
    assert(leancast::extensions::PathInsideDirectory(manifest.manifest->dllPath, pluginDir));

    const auto missing = ParseManifestJson("{\"id\":\"bad\",\"name\":\"Bad\",\"dll\":\"bad.dll\"}",
                                           pluginDir / L"missing.json");
    assert(!missing.manifest);

    const auto escaping = ParseManifestJson("{\"id\":\"bad/path\",\"name\":\"Bad\",\"version\":\"1\",\"dll\":\"bad.dll\"}",
                                            pluginDir / L"bad-id.json");
    assert(!escaping.manifest);

    const auto outside = ParseManifestJson("{\"id\":\"escape\",\"name\":\"Escape\",\"version\":\"1\",\"dll\":\"..\\\\escape.dll\"}",
                                           pluginDir / L"escape.json");
    assert(!outside.manifest);

    const auto exePluginDir = tempRoot / L"exe" / L"plugins" / L"sample";
    WriteUtf8(exePluginDir / L"plugin.json",
              "{\"id\":\"sample\",\"name\":\"Bundled Sample\",\"version\":\"1.0\",\"dll\":\"bundled.dll\"}");
    const auto discovered = DiscoverManifests(tempRoot / L"data", tempRoot / L"exe");
    assert(discovered.manifests.size() == 1);
    assert(discovered.manifests.front().name == L"Sample");

    assert(ResponseSizeAllowed(1));
    assert(ResponseSizeAllowed(leancast::extensions::kMaxResponseBytes));
    assert(!ResponseSizeAllowed(0));
    assert(!ResponseSizeAllowed(leancast::extensions::kMaxResponseBytes + 1));

    const auto query = ParseQueryResponse(
        "{\"items\":[{\"id\":\"one\",\"title\":\"One\",\"subtitle\":\"From plugin\","
        "\"keywords\":[\"uno\",\"first\"],\"score\":42,\"iconPath\":\"C:/icon.png\","
        "\"payload\":{\"answer\":1}}]}");
    assert(query && query->items.size() == 1);
    assert(query->items.front().id == L"one");
    assert(query->items.front().title == L"One");
    assert(query->items.front().subtitle == L"From plugin");
    assert(query->items.front().keywords.size() == 2);
    assert(query->items.front().score == 42.0);
    assert(query->items.front().payloadJson.find("\"answer\"") != std::string::npos);
    assert(!ParseQueryResponse("{}"));

    const auto activation = ParseActivationResponse(
        "{\"handled\":true,\"closeOverlay\":false,\"action\":{\"type\":\"copyText\",\"value\":\"copied\"}}");
    assert(activation);
    assert(activation->handled);
    assert(!activation->closeOverlay);
    assert(activation->action == HostActionType::CopyText);
    assert(activation->value == L"copied");

    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto snippets = ParseSnippetsJson(
        "{\"snippets\":["
        "{\"keyword\":\"sig\",\"name\":\"Email Signature\",\"text\":\"Best,\\nLeon\"},"
        "{\"keyword\":\"\",\"name\":\"Missing Keyword\",\"text\":\"ignored\"},"
        "{\"keyword\":\"bad\",\"name\":\"Missing Text\"},"
        "{\"keyword\":\"empty\",\"name\":\"Empty Text\",\"text\":\"   \"}"
        "]}");
    assert(snippets.size() == 1);
    assert(snippets.front().keyword == L"sig");
    assert(snippets.front().name == L"Email Signature");
    assert(snippets.front().text == L"Best,\nLeon");
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"LeanCastPhase5StorageTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    leancast::storage::Storage storage;
    assert(storage.Open(tempRoot / L"leancast.db"));

    std::vector<leancast::storage::FileIndexEntry> files = {
      {L"C:\\Users\\Leon\\Documents\\Notes", L"Notes", true, L"C:\\Users\\Leon\\Documents\\Notes", 10, 0, 100},
      {L"C:\\Users\\Leon\\Downloads\\setup.exe", L"setup.exe", false, L"C:\\Users\\Leon\\Downloads\\setup.exe", 11, 42, 100},
    };
    assert(storage.ReplaceFileIndex(files));
    const auto loadedFiles = storage.LoadFileIndex();
    assert(loadedFiles.size() == 2);
    assert(loadedFiles.front().name == L"Notes");
    assert(loadedFiles.front().isDirectory);

    assert(storage.AddClipboardEntry(L"first", L"first", 1, 2));
    assert(storage.AddClipboardEntry(L"second", L"second", 2, 2));
    assert(storage.AddClipboardEntry(L"first", L"first", 3, 2));
    const auto clips = storage.LoadClipboardHistory(10);
    assert(clips.size() == 2);
    assert(clips.front().text == L"first");
    assert(clips.front().capturedAt == 3);
    assert(clips.back().text == L"second");
    assert(storage.ClearClipboardHistory());
    assert(storage.LoadClipboardHistory(10).empty());

    storage.Close();
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto parsed = leancast::theme::ParseThemeJson(
        "{\"fontFamily\":\"Cascadia Mono\","
        "\"overlayBackground\":\"#11223380\","
        "\"textPrimary\":\"not-a-color\","
        "\"rowRadius\":12,"
        "\"controlRadius\":500}");
    assert(parsed.fontFamily == L"Cascadia Mono");
    assert(std::fabs(parsed.overlayBackground.r - (0x11 / 255.0f)) < 0.001);
    assert(std::fabs(parsed.overlayBackground.a - (0x80 / 255.0f)) < 0.001);
    assert(std::fabs(parsed.textPrimary.r - leancast::theme::Theme{}.textPrimary.r) < 0.001);
    assert(parsed.rowRadius == 12.0f);
    assert(parsed.controlRadius == 20.0f);

    const auto color = leancast::theme::ParseHexColor(L"#ABCDEF");
    assert(color);
    assert(std::fabs(color->g - (0xCD / 255.0f)) < 0.001);
    assert(!leancast::theme::ParseHexColor(L"#NOPE"));
  }

  {
    const auto url = leancast::run_command::Classify(L">example.com/path");
    assert(url);
    assert(url->kind == leancast::run_command::Kind::OpenTarget);
    assert(url->target == L"https://example.com/path");

    const auto scheme = leancast::run_command::Classify(L">ms-settings:display");
    assert(scheme);
    assert(scheme->kind == leancast::run_command::Kind::OpenTarget);
    assert(scheme->target == L"ms-settings:display");

    const auto pathRoot = std::filesystem::temp_directory_path() / L"LeanCastRunCommandTests";
    std::filesystem::create_directories(pathRoot);
    const auto pathCommand = leancast::run_command::Classify(L">\"" + pathRoot.wstring() + L"\"");
    assert(pathCommand);
    assert(pathCommand->kind == leancast::run_command::Kind::OpenTarget);
    assert(pathCommand->target == pathRoot.wstring());
    std::error_code ec;
    std::filesystem::remove_all(pathRoot, ec);

    const auto shell = leancast::run_command::Classify(L">echo hello");
    assert(shell);
    assert(shell->kind == leancast::run_command::Kind::ShellCommand);
    assert(shell->input == L"echo hello");
  }

  {
    const auto arrows = leancast::symbols::SearchSymbols(L":arrow", 5);
    assert(!arrows.empty());
    assert(arrows.front().value == L"\u2192" || arrows.front().label.find(L"Arrow") != std::wstring::npos);

    const auto checks = leancast::symbols::SearchSymbols(L":check", 5);
    assert(!checks.empty());
    assert(checks.front().label.find(L"Check") != std::wstring::npos);

    const auto smiles = leancast::symbols::SearchSymbols(L":smile", 5);
    assert(!smiles.empty());
    assert(smiles.front().label.find(L"Face") != std::wstring::npos);
  }

  {
    // The generated emoji table must be present and searchable.
    assert(!leancast::emoji::AllEmoji().empty());
    for (const auto& emoji : leancast::emoji::AllEmoji()) {
      assert(!emoji.value.empty());
      assert(!emoji.label.empty());
    }

    const auto empty = leancast::emoji::SearchEmoji(L"", 10);
    assert(empty.size() == 10);

    const auto smile = leancast::emoji::SearchEmoji(L"smile", 5);
    assert(!smile.empty());
    assert(smile.front().label.find(L"smil") != std::wstring::npos ||
           smile.front().label.find(L"grin") != std::wstring::npos);

    const auto fire = leancast::emoji::SearchEmoji(L"fire", 5);
    assert(!fire.empty());
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

  SearchItem snippet;
  snippet.id = L"snippet:sig";
  snippet.kind = L"snippet";
  snippet.source = L"snippet";
  snippet.name = L"Email Signature";
  snippet.keywords = {L"sig", L"Best,\nLeon"};

  SearchItem clipboard;
  clipboard.id = L"clipboard:1";
  clipboard.kind = L"clipboard";
  clipboard.source = L"clipboard";
  clipboard.name = L"Clipboard Text";
  clipboard.keywords = {L"recent copied value"};

  std::vector<SearchItem> items = {notepad, terminal, window, snippet, clipboard};
  const auto shell = Search(L"shell", items);
  assert(!shell.empty() && shell.front() == 1);

  const auto acronym = Search(L"wt", items);
  assert(!acronym.empty() && acronym.front() == 1);

  const auto cpp = Search(L"studio", items);
  assert(!cpp.empty() && cpp.front() == 2);

  const auto snippetHit = Search(L"sig", items);
  assert(!snippetHit.empty() && snippetHit.front() == 3);

  const auto clipboardHit = Search(L"copied value", items);
  assert(!clipboardHit.empty() && clipboardHit.front() == 4);

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
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Alt+Space");
  }

  {
    const auto altSpaceHotKey = ToHotKeySpec(ParseShortcut(L"Alt+Space"));
    assert(altSpaceHotKey.supported);
    assert(altSpaceHotKey.vk == VK_SPACE);
    assert((altSpaceHotKey.modifiers & MOD_ALT) != 0);
    assert((altSpaceHotKey.modifiers & MOD_CONTROL) == 0);
    assert((altSpaceHotKey.modifiers & 0x4000) != 0);

    const auto controlAltKHotKey = ToHotKeySpec(ParseShortcut(L"Control+Alt+K"));
    assert(controlAltKHotKey.supported);
    assert(controlAltKHotKey.vk == L'K');
    assert((controlAltKHotKey.modifiers & MOD_CONTROL) != 0);
    assert((controlAltKHotKey.modifiers & MOD_ALT) != 0);

    assert(!ToHotKeySpec(ParseShortcut(L"Super")).supported);
    assert(!ToHotKeySpec(ParseShortcut(L"none")).supported);

    assert(!ShouldHandleInLowLevelHook(ParseShortcut(L"Alt+Space"), true));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Alt+Space"), false));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Super"), false));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Super"), true));
    assert(!ShouldHandleInLowLevelHook(ParseShortcut(L"none"), false));
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_CONTROL, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Control+Space");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_CONTROL, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(L'K', true, false), L"Control+Alt+K");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_LWIN, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Super+Space");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_LWIN, true, false));
    AssertRecorded(recorder.Handle(VK_LWIN, false, true), L"Super");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingCanceled(recorder.Handle(VK_ESCAPE, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(VK_MENU, false, true), L"Alt");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(L'K', true, false));
    AssertRecordingPending(recorder.Handle(VK_CAPITAL, true, false));
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecordingPending(recorder.Handle(VK_CAPITAL, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, false, true));
  }

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
    assert(open.suppressWinStart);
    const auto repeat = runtime.Handle(superSpace, VK_SPACE, true, false, Mods(false, false, false, true));
    assert(repeat.consume);
    assert(!repeat.toggle);
    const auto release = runtime.Handle(superSpace, VK_SPACE, false, true, Mods(false, false, false, true));
    assert(release.consume);
    assert(!release.toggle);
    assert(!release.suppressWinStart);
  }

  {
    const auto altSpace = ParseShortcut(L"Alt+Space");
    ShortcutRuntime runtime;
    const auto open = runtime.Handle(altSpace, VK_SPACE, true, false, Mods(false, true));
    assert(open.consume);
    assert(open.toggle);
    const auto altSpaceRelease = runtime.Handle(altSpace, VK_SPACE, false, true, Mods(false, true));
    assert(altSpaceRelease.consume);
    assert(!altSpaceRelease.toggle);

    AssertPassOnly(runtime.Handle(altSpace, VK_RETURN, true, false, Mods(false, true)));

    const auto controlSpace = ParseShortcut(L"Control+Space");
    ShortcutRuntime controlRuntime;
    const auto controlOpen = controlRuntime.Handle(controlSpace, VK_SPACE, true, false, Mods(true));
    assert(controlOpen.consume);
    assert(controlOpen.toggle);
    const auto controlRelease = controlRuntime.Handle(controlSpace, VK_SPACE, false, true, Mods(true));
    assert(controlRelease.consume);
    assert(!controlRelease.toggle);

    const auto alt = ParseShortcut(L"Alt");
    ShortcutRuntime altRuntime;
    AssertPassOnly(altRuntime.Handle(alt, VK_MENU, true, false, Mods(false, true)));
    const auto altRelease = altRuntime.Handle(alt, VK_MENU, false, true, Mods());
    assert(altRelease.consume);
    assert(altRelease.toggle);
    assert(!altRelease.suppressWinStart);
  }

  return 0;
}
