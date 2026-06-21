#pragma once

#include "extension_protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace leancast::extensions {

class ExtensionManager {
 public:
  ExtensionManager() = default;
  ~ExtensionManager() { Shutdown(); }

  ExtensionManager(const ExtensionManager&) = delete;
  ExtensionManager& operator=(const ExtensionManager&) = delete;

  void Initialize(std::filesystem::path dataDir, std::filesystem::path exeDir, HWND notifyHwnd, UINT notifyMessage) {
    dataDir_ = std::move(dataDir);
    exeDir_ = std::move(exeDir);
    notifyHwnd_ = notifyHwnd;
    notifyMessage_ = notifyMessage;
    Reload();
    StartQueryWorker();
  }

  void OnBackground() {
    {
      std::lock_guard cacheLock(cacheMutex_);
      cache_.clear();
    }
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& plugin : plugins_) {
        std::lock_guard ioLock(plugin->ioMutex);
        StopProcess(*plugin);
      }
    }
  }

  void Shutdown() {
    {
      std::lock_guard lock(queryMutex_);
      stop_ = true;
      pendingQuery_.reset();
    }
    queryCv_.notify_all();
    if (queryThread_.joinable()) {
      queryThread_.request_stop();
      queryThread_.join();
    }

    std::lock_guard pluginsLock(pluginsMutex_);
    for (const auto& plugin : plugins_) {
      plugin->available = false;
      std::lock_guard ioLock(plugin->ioMutex);
      StopProcess(*plugin);
    }
    plugins_.clear();
  }

  void Reload() {
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& plugin : plugins_) {
        plugin->available = false;
        std::lock_guard ioLock(plugin->ioMutex);
        StopProcess(*plugin);
      }
      plugins_.clear();

      auto discovery = DiscoverManifests(dataDir_, exeDir_);
      for (const auto& error : discovery.errors) Log(error);
      for (auto& manifest : discovery.manifests) {
        auto plugin = std::make_shared<Plugin>();
        plugin->manifest = std::move(manifest);
        plugins_.push_back(std::move(plugin));
      }
      Log(L"Loaded " + std::to_wstring(plugins_.size()) + L" extension(s)");
    }

    {
      std::lock_guard cacheLock(cacheMutex_);
      cache_.clear();
    }
  }

  void RequestQuery(std::wstring query, unsigned long long generation) {
    if (TrimWide(query).empty()) return;
    {
      std::lock_guard cacheLock(cacheMutex_);
      if (cache_.contains(query)) return;
    }

    {
      std::lock_guard queryLock(queryMutex_);
      if (runningQuery_ == query) {
        latestRequestedGeneration_ = generation;
        return;
      }
      if (pendingQuery_ && pendingQuery_->query == query) {
        pendingQuery_->generation = generation;
        latestRequestedGeneration_ = generation;
        return;
      }
      pendingQuery_ = PendingQuery{std::move(query), generation};
      latestRequestedGeneration_ = generation;
    }
    queryCv_.notify_one();
  }

  std::vector<QueryResultItem> CachedResultsFor(const std::wstring& query) const {
    std::lock_guard cacheLock(cacheMutex_);
    if (const auto found = cache_.find(query); found != cache_.end()) return found->second;
    return {};
  }

  std::optional<ActivationResponse> Activate(const QueryResultItem& item,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::shared_ptr<Plugin> plugin;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& candidate : plugins_) {
        if (candidate->manifest.id == item.pluginId) {
          plugin = candidate;
          break;
        }
      }
    }
    if (!plugin || !plugin->available) return std::nullopt;

    std::string response;
    if (!SendRequest(*plugin, BuildActivateRequestJson(plugin->manifest, dataDir_, item), timeout, response)) {
      return std::nullopt;
    }
    return ParseActivationResponse(response);
  }

 private:
  struct Plugin {
    Manifest manifest;
    bool available = true;
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    std::mutex ioMutex;
  };

  struct PendingQuery {
    std::wstring query;
    unsigned long long generation = 0;
  };

  static std::wstring TrimWide(std::wstring value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
    if (first >= last) return L"";
    return std::wstring(first, last);
  }

  static void CloseHandleIfSet(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = nullptr;
    }
  }

  static std::wstring QuoteCommandArg(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring out = L"\"";
    for (const wchar_t ch : value) {
      if (ch == L'"') out += L"\\\"";
      else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
  }

  static bool ProcessRunning(HANDLE process) {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  }

  void StartQueryWorker() {
    if (queryThread_.joinable()) return;
    queryThread_ = std::jthread([this](std::stop_token stopToken) {
      for (;;) {
        PendingQuery pending;
        {
          std::unique_lock lock(queryMutex_);
          queryCv_.wait(lock, [&] {
            return pendingQuery_.has_value() || stop_ || stopToken.stop_requested();
          });
          if (stop_ || stopToken.stop_requested()) return;
          pending = std::move(*pendingQuery_);
          pendingQuery_.reset();
          runningQuery_ = pending.query;
        }

        auto results = QueryPlugins(pending.query, stopToken);
        {
          std::lock_guard cacheLock(cacheMutex_);
          if (cache_.size() > 32) cache_.clear();
          cache_[pending.query] = std::move(results);
        }

        bool newest = false;
        {
          std::lock_guard queryLock(queryMutex_);
          newest = pending.generation == latestRequestedGeneration_;
          if (runningQuery_ == pending.query) runningQuery_.clear();
        }
        if (newest && notifyHwnd_) PostMessageW(notifyHwnd_, notifyMessage_, 0, 0);
      }
    });
  }

  std::vector<QueryResultItem> QueryPlugins(const std::wstring& query, std::stop_token stopToken) {
    std::vector<QueryResultItem> results;
    std::lock_guard pluginsLock(pluginsMutex_);
    for (const auto& plugin : plugins_) {
      if (stopToken.stop_requested() || stop_) break;
      if (!plugin->available) continue;

      std::string response;
      if (!SendRequest(*plugin, BuildQueryRequestJson(plugin->manifest, dataDir_, query, kDefaultQueryLimit),
                       std::chrono::milliseconds(250), response)) {
        continue;
      }

      auto parsed = ParseQueryResponse(response, kDefaultQueryLimit);
      if (!parsed) {
        Log(plugin->manifest.id + L": invalid query response");
        continue;
      }
      for (auto& item : parsed->items) {
        item.pluginId = plugin->manifest.id;
        item.pluginName = plugin->manifest.name;
        results.push_back(std::move(item));
      }
    }

    std::sort(results.begin(), results.end(), [](const QueryResultItem& a, const QueryResultItem& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.title < b.title;
    });
    return results;
  }

  bool EnsureProcess(Plugin& plugin) {
    if (!plugin.available) return false;
    if (ProcessRunning(plugin.process)) return true;

    StopProcess(plugin);

    const auto hostPath = exeDir_ / L"LeanCastPluginHost.exe";
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStderr = nullptr;

    if (!CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0) ||
        !CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0)) {
      CloseHandleIfSet(childStdinRead);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      CloseHandleIfSet(childStdoutWrite);
      MarkUnavailable(plugin, L"failed to create plugin host pipes");
      return false;
    }

    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);
    childStderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStderr ? childStderr : childStdoutWrite;

    PROCESS_INFORMATION process{};
    std::wstring command = QuoteCommandArg(hostPath) + L" " + QuoteCommandArg(plugin.manifest.dllPath);
    const BOOL created = CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    CloseHandleIfSet(childStdinRead);
    CloseHandleIfSet(childStdoutWrite);
    CloseHandleIfSet(childStderr);

    if (!created) {
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to start LeanCastPluginHost.exe");
      return false;
    }

    CloseHandleIfSet(process.hThread);
    plugin.process = process.hProcess;
    plugin.stdinWrite = parentStdinWrite;
    plugin.stdoutRead = parentStdoutRead;
    return true;
  }

  bool SendRequest(Plugin& plugin, const std::string& request, std::chrono::milliseconds timeout,
                   std::string& response) {
    std::lock_guard ioLock(plugin.ioMutex);
    if (!EnsureProcess(plugin)) return false;

    const std::string line = request + "\n";
    DWORD written = 0;
    if (!WriteFile(plugin.stdinWrite, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size()) {
      MarkUnavailable(plugin, L"plugin host write failed");
      return false;
    }

    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!ProcessRunning(plugin.process)) {
        MarkUnavailable(plugin, L"plugin host exited unexpectedly");
        return false;
      }

      DWORD available = 0;
      if (!PeekNamedPipe(plugin.stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
        MarkUnavailable(plugin, L"plugin host read failed");
        return false;
      }

      if (available > 0) {
        char chunk[4096]{};
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
        DWORD read = 0;
        if (!ReadFile(plugin.stdoutRead, chunk, toRead, &read, nullptr)) {
          MarkUnavailable(plugin, L"plugin host read failed");
          return false;
        }
        buffer.append(chunk, chunk + read);
        if (buffer.size() > kMaxResponseBytes) {
          MarkUnavailable(plugin, L"plugin host response exceeded 1 MiB");
          return false;
        }
        if (const size_t newline = buffer.find('\n'); newline != std::string::npos) {
          response = buffer.substr(0, newline);
          if (!response.empty() && response.back() == '\r') response.pop_back();
          return true;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    MarkUnavailable(plugin, L"plugin host timed out");
    return false;
  }

  void MarkUnavailable(Plugin& plugin, const std::wstring& reason) {
    Log(plugin.manifest.id + L": " + reason);
    plugin.available = false;
    StopProcess(plugin);
  }

  void StopProcess(Plugin& plugin) {
    CloseHandleIfSet(plugin.stdinWrite);
    CloseHandleIfSet(plugin.stdoutRead);
    if (plugin.process) {
      if (WaitForSingleObject(plugin.process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(plugin.process, 0);
        WaitForSingleObject(plugin.process, 1000);
      }
      CloseHandleIfSet(plugin.process);
    }
  }

  void Log(const std::wstring& message) const {
    if (dataDir_.empty()) return;
    std::lock_guard lock(logMutex_);
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    std::ofstream file(dataDir_ / L"extension-log.txt", std::ios::binary | std::ios::app);
    if (!file) return;
    const auto now = static_cast<long long>(std::time(nullptr));
    file << now << " " << WideToUtf8(message) << "\n";
  }

  std::filesystem::path dataDir_;
  std::filesystem::path exeDir_;
  HWND notifyHwnd_ = nullptr;
  UINT notifyMessage_ = 0;

  mutable std::mutex logMutex_;
  mutable std::mutex cacheMutex_;
  std::map<std::wstring, std::vector<QueryResultItem>> cache_;

  std::mutex pluginsMutex_;
  std::vector<std::shared_ptr<Plugin>> plugins_;

  std::jthread queryThread_;
  std::mutex queryMutex_;
  std::condition_variable queryCv_;
  std::optional<PendingQuery> pendingQuery_;
  std::wstring runningQuery_;
  unsigned long long latestRequestedGeneration_ = 0;
  bool stop_ = false;
};

}  // namespace leancast::extensions
