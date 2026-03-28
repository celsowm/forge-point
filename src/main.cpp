#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace ftxui;

namespace {

constexpr const char* kAppTitle = "Forge-Point";
constexpr const char* kTagline = "Local-first GGUF cockpit for llama.cpp";
constexpr size_t kMaxLogs = 400;

std::string Trim(std::string value) {
  auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
  return value;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix) {
  return ToLower(value).rfind(ToLower(prefix), 0) == 0;
}

std::string ReplaceAll(std::string value, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

std::string UrlEncode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex << std::uppercase;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      escaped << c;
    } else {
      escaped << '%' << std::setw(2) << int(c);
    }
  }
  return escaped.str();
}

std::string HumanBytes(std::uintmax_t bytes) {
  static const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  double count = static_cast<double>(bytes);
  size_t i = 0;
  while (count >= 1024.0 && i + 1 < std::size(suffixes)) {
    count /= 1024.0;
    ++i;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(i == 0 ? 0 : 1) << count << ' ' << suffixes[i];
  return oss.str();
}

std::string GetEnv(const char* key) {
  if (const char* value = std::getenv(key)) {
    return value;
  }
  return {};
}

std::optional<fs::path> HomeDirectory() {
#ifdef _WIN32
  const std::string user_profile = GetEnv("USERPROFILE");
  if (!user_profile.empty()) {
    return fs::path(user_profile);
  }
  const std::string home_drive = GetEnv("HOMEDRIVE");
  const std::string home_path = GetEnv("HOMEPATH");
  if (!home_drive.empty() && !home_path.empty()) {
    return fs::path(home_drive + home_path);
  }
  return std::nullopt;
#else
  const std::string home = GetEnv("HOME");
  if (!home.empty()) {
    return fs::path(home);
  }
  return std::nullopt;
#endif
}

fs::path DefaultHfCacheRoot() {
  const std::string hf_hub_cache = GetEnv("HF_HUB_CACHE");
  if (!hf_hub_cache.empty()) {
    return fs::path(hf_hub_cache);
  }
  const std::string hf_home = GetEnv("HF_HOME");
  if (!hf_home.empty()) {
    return fs::path(hf_home) / "hub";
  }
#ifdef _WIN32
  const std::string local_app_data = GetEnv("LOCALAPPDATA");
  if (!local_app_data.empty()) {
    const fs::path win32_cache = fs::path(local_app_data) / "huggingface" / "hub";
    if (fs::exists(win32_cache)) {
      return win32_cache;
    }
  }
  if (const auto home = HomeDirectory()) {
    const fs::path win_cache = *home / ".cache" / "huggingface" / "hub";
    if (fs::exists(win_cache)) {
      return win_cache;
    }
  }
#endif
  const std::string xdg_cache_home = GetEnv("XDG_CACHE_HOME");
  if (!xdg_cache_home.empty()) {
    return fs::path(xdg_cache_home) / "huggingface" / "hub";
  }
  if (const auto home = HomeDirectory()) {
    return *home / ".cache" / "huggingface" / "hub";
  }
  return fs::current_path() / ".cache" / "huggingface" / "hub";
}

std::vector<std::string> SplitArgs(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  bool in_quotes = false;
  char quote_char = '\0';
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if ((ch == '\'' || ch == '"') && (!in_quotes || quote_char == ch)) {
      if (in_quotes) {
        in_quotes = false;
        quote_char = '\0';
      } else {
        in_quotes = true;
        quote_char = ch;
      }
      continue;
    }
    if (!in_quotes && std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current += ch;
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

std::vector<std::string> SplitWords(const std::string& text) {
  std::istringstream iss(text);
  std::vector<std::string> out;
  std::string item;
  while (iss >> item) {
    out.push_back(item);
  }
  return out;
}

std::string ShellQuote(const std::string& arg) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') out += '\\';
    out += c;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
#endif
}

std::string JoinCommand(const std::vector<std::string>& argv) {
  std::ostringstream oss;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) oss << ' ';
    oss << ShellQuote(argv[i]);
  }
  return oss.str();
}

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string error;
};

using ProgressFn = std::function<void(uint64_t downloaded, uint64_t total)>;

class HttpClient {
 public:
  HttpClient() {}
  ~HttpClient() {}

  HttpResponse Get(const std::string& url, const std::vector<std::string>& headers = {}) const {
    HttpResponse response;
    response = WinHttpGet(url, headers);
    return response;
  }

  bool DownloadToFile(const std::string& url,
                      const fs::path& target,
                      std::string& error,
                      const std::vector<std::string>& headers = {},
                      ProgressFn on_progress = nullptr) const {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    const fs::path part_file = fs::path(target.string() + ".part");
    std::ofstream out(part_file, std::ios::binary);
    if (!out) {
      error = "failed to open destination file";
      return false;
    }

    bool ok = StreamDownload(url, headers, out, error, std::move(on_progress));
    out.close();

    if (!ok) {
      fs::remove(part_file, ec);
      return false;
    }

    fs::rename(part_file, target, ec);
    if (ec) {
      fs::remove(part_file, ec);
      error = "failed to rename .part file";
      return false;
    }
    return true;
  }

 private:
#ifdef _WIN32
  struct WinHttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    ~WinHttpHandles() {
      if (request) WinHttpCloseHandle(request);
      if (connect) WinHttpCloseHandle(connect);
      if (session) WinHttpCloseHandle(session);
    }
  };

  static bool WinHttpSetup(const std::string& url,
                            const std::vector<std::string>& headers,
                            WinHttpHandles& h,
                            long& status_code,
                            std::string& error) {
    URL_COMPONENTSW urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = 1;
    urlComp.dwUrlPathLength = 1;
    urlComp.dwExtraInfoLength = 1;

    std::wstring wide_url(url.begin(), url.end());
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &urlComp)) {
      error = "WinHttpCrackUrl failed";
      return false;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    if (urlComp.lpszExtraInfo && urlComp.dwExtraInfoLength > 0) {
      path += std::wstring(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
    }

    h.session = WinHttpOpen(L"forge-point/0.1",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) { error = "WinHttpOpen failed"; return false; }

    h.connect = WinHttpConnect(h.session, host.c_str(), urlComp.nPort, 0);
    if (!h.connect) { error = "WinHttpConnect failed"; return false; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    h.request = WinHttpOpenRequest(h.connect, L"GET", path.c_str(),
                                   nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h.request) { error = "WinHttpOpenRequest failed"; return false; }

    for (const auto& hdr : headers) {
      std::wstring wh(hdr.begin(), hdr.end());
      WinHttpAddRequestHeaders(h.request, wh.c_str(),
                               static_cast<DWORD>(wh.size()),
                               WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      error = "WinHttpSendRequest failed";
      return false;
    }

    if (!WinHttpReceiveResponse(h.request, nullptr)) {
      error = "WinHttpReceiveResponse failed";
      return false;
    }

    DWORD sc = 0;
    DWORD sz = sizeof(sc);
    WinHttpQueryHeaders(h.request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &sc, &sz, WINHTTP_NO_HEADER_INDEX);
    status_code = static_cast<long>(sc);
    return true;
  }

  static uint64_t WinHttpContentLength(HINTERNET hRequest) {
    wchar_t buf[64] = {};
    DWORD buf_size = sizeof(buf);
    if (WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            buf, &buf_size, WINHTTP_NO_HEADER_INDEX)) {
      try { return std::stoull(std::wstring(buf)); } catch (...) {}
    }
    return 0;
  }

  static HttpResponse WinHttpGet(const std::string& url, const std::vector<std::string>& headers) {
    HttpResponse response;
    WinHttpHandles h;
    if (!WinHttpSetup(url, headers, h, response.status, response.error)) {
      return response;
    }

    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(h.request, &bytesAvailable) && bytesAvailable > 0) {
      std::string buffer(bytesAvailable, '\0');
      DWORD bytesRead = 0;
      if (WinHttpReadData(h.request, &buffer[0], bytesAvailable, &bytesRead)) {
        response.body.append(buffer.data(), bytesRead);
      }
      bytesAvailable = 0;
    }
    return response;
  }

  static bool StreamDownload(const std::string& url,
                             const std::vector<std::string>& headers,
                             std::ostream& out,
                             std::string& error,
                             ProgressFn on_progress) {
    WinHttpHandles h;
    long status = 0;
    if (!WinHttpSetup(url, headers, h, status, error)) return false;
    if (status < 200 || status >= 300) {
      error = "HTTP " + std::to_string(status);
      return false;
    }

    uint64_t total = WinHttpContentLength(h.request);
    uint64_t downloaded = 0;
    auto last_report = std::chrono::steady_clock::now();

    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(h.request, &bytesAvailable) && bytesAvailable > 0) {
      std::string buffer(bytesAvailable, '\0');
      DWORD bytesRead = 0;
      if (WinHttpReadData(h.request, &buffer[0], bytesAvailable, &bytesRead)) {
        out.write(buffer.data(), bytesRead);
        downloaded += bytesRead;

        auto now = std::chrono::steady_clock::now();
        if (on_progress && (now - last_report > std::chrono::milliseconds(250) || bytesAvailable == 0)) {
          on_progress(downloaded, total);
          last_report = now;
        }
      }
      bytesAvailable = 0;
    }
    if (on_progress) on_progress(downloaded, total);
    return true;
  }

#else
  static HttpResponse WinHttpGet(const std::string& url, const std::vector<std::string>& headers) {
    HttpResponse response;
    std::string header_args;
    for (const auto& h : headers) {
      header_args += " -H " + ShellQuote(h);
    }
    std::string cmd = "curl -s -o - -w '\\n%{http_code}'" + header_args + " " + ShellQuote(url);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      response.error = "failed to run curl";
      return response;
    }
    std::string all_output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
      all_output += buf;
    }
    int status = pclose(pipe);
    
    auto last_nl = all_output.rfind('\n');
    if (last_nl != std::string::npos) {
      std::string code_str = Trim(all_output.substr(last_nl + 1));
      try { response.status = std::stol(code_str); } catch (...) {}
      response.body = all_output.substr(0, last_nl);
    } else {
      response.body = all_output;
    }
    return response;
  }

  static bool StreamDownload(const std::string& url,
                             const std::vector<std::string>& headers,
                             std::ostream& out,
                             std::string& error,
                             ProgressFn on_progress) {
    std::string header_args;
    for (const auto& h : headers) {
      header_args += " -H " + ShellQuote(h);
    }
    std::string cmd = "curl -s -L -o -" + header_args + " " + ShellQuote(url);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      error = "failed to run curl";
      return false;
    }
    uint64_t downloaded = 0;
    auto last_report = std::chrono::steady_clock::now();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
      out.write(buf, static_cast<std::streamsize>(n));
      downloaded += n;
      auto now = std::chrono::steady_clock::now();
      if (on_progress && (now - last_report > std::chrono::milliseconds(250))) {
        on_progress(downloaded, 0);
        last_report = now;
      }
    }
    int status = pclose(pipe);
    if (on_progress) on_progress(downloaded, 0);
    if (status != 0) {
      error = "curl failed with exit code " + std::to_string(status);
      return false;
    }
    return true;
  }
#endif
};

struct HfRepo {
  std::string id;
  std::string sha;
  int downloads = 0;
  int likes = 0;
};

struct HfFile {
  std::string filename;
  std::uintmax_t size = 0;
  std::string download_url;
};

struct LocalModel {
  std::string name;
  fs::path path;
  std::uintmax_t size = 0;
  std::string origin;
};

class HfClient {
 public:
  explicit HfClient(HttpClient& http) : http_(http) {}

  std::vector<HfRepo> SearchRepos(const std::string& query, std::string& error) {
    const std::string url =
        "https://huggingface.co/api/models?search=" + UrlEncode(query) +
        "&limit=20&sort=downloads&direction=-1";
    auto response = GetWithAuth(url);
    if (!response.error.empty()) {
      error = response.error;
      return {};
    }
    if (response.status < 200 || response.status >= 300) {
      error = "HTTP " + std::to_string(response.status);
      return {};
    }

    const auto payload = json::parse(response.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_array()) {
      error = "invalid JSON from Hugging Face search API";
      return {};
    }

    std::vector<HfRepo> repos;
    for (const auto& item : payload) {
      HfRepo repo;
      repo.id = item.value("id", "");
      repo.sha = item.value("sha", "");
      repo.downloads = item.value("downloads", 0);
      repo.likes = item.value("likes", 0);
      if (!repo.id.empty()) {
        repos.push_back(std::move(repo));
      }
    }
    return repos;
  }

  std::vector<HfFile> ListGgufFiles(const std::string& repo_id, std::string& repo_sha, std::string& error) {
    const std::string url = "https://huggingface.co/api/models/" + UrlEncode(repo_id);
    auto response = GetWithAuth(url);
    if (!response.error.empty()) {
      error = response.error;
      return {};
    }
    if (response.status < 200 || response.status >= 300) {
      error = "HTTP " + std::to_string(response.status);
      return {};
    }

    const auto payload = json::parse(response.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      error = "invalid JSON from Hugging Face model API";
      return {};
    }

    repo_sha = payload.value("sha", repo_sha);
    std::vector<HfFile> files;
    for (const auto& sibling : payload.value("siblings", json::array())) {
      const std::string name = sibling.value("rfilename", "");
      if (name.size() < 5 || !StartsWithIgnoreCase(name.substr(name.size() - 5), ".gguf")) {
        continue;
      }
      HfFile file;
      file.filename = name;
      file.size = sibling.value("size", 0ULL);
      const std::string ref = repo_sha.empty() ? "main" : repo_sha;
      file.download_url = "https://huggingface.co/" + repo_id + "/resolve/" + ref + "/" + UrlEncode(name) + "?download=true";
      files.push_back(std::move(file));
    }

    std::sort(files.begin(), files.end(), [](const HfFile& a, const HfFile& b) {
      return a.filename < b.filename;
    });
    return files;
  }

  bool DownloadFile(const std::string& url, const fs::path& target, std::string& error, ProgressFn on_progress = nullptr) {
    return http_.DownloadToFile(url, target, error, AuthHeaders(), std::move(on_progress));
  }

 private:
  std::vector<std::string> AuthHeaders() const {
    const std::string token = GetEnv("HF_TOKEN");
    if (token.empty()) return {};
    return {"Authorization: Bearer " + token};
  }

  HttpResponse GetWithAuth(const std::string& url) const {
    return http_.Get(url, AuthHeaders());
  }

  HttpClient& http_;
};

class HfCacheScanner {
 public:
  explicit HfCacheScanner(fs::path app_models_dir) : app_models_dir_(std::move(app_models_dir)) {}

  fs::path CacheRoot() const { return DefaultHfCacheRoot(); }

  std::vector<LocalModel> Scan() const {
    std::vector<LocalModel> models;
    std::set<std::string> seen;

    auto add_model = [&](const fs::path& path, const std::string& origin) {
      std::error_code ec;
      const auto extension = ToLower(path.extension().string());
      if (extension != ".gguf") return;

      const auto canonical_path = fs::weakly_canonical(path, ec);
      const std::string stable_path = ec ? path.lexically_normal().string() : canonical_path.string();
      if (!seen.insert(stable_path).second) return;

      LocalModel model;
      model.name = path.filename().string();
      model.path = path;
      model.origin = origin;
      model.size = fs::file_size(path, ec);
      models.push_back(std::move(model));
    };

    auto scan_tree = [&](const fs::path& root, const std::string& origin) {
      if (!fs::exists(root)) return;
      fs::directory_options options = fs::directory_options::skip_permission_denied;
      for (fs::recursive_directory_iterator it(root, options), end; it != end; ++it) {
        std::error_code ec;
        const auto status = it->symlink_status(ec);
        if (ec) continue;
        if (!fs::is_regular_file(status) && !fs::is_symlink(status)) continue;
        add_model(it->path(), origin);
      }
    };

    auto scan_hf_cache = [&](const fs::path& cache_root) {
      if (!fs::exists(cache_root)) return;
      fs::directory_options options = fs::directory_options::skip_permission_denied;
      for (fs::directory_iterator it(cache_root, options), end; it != end; ++it) {
        std::error_code ec;
        if (!it->is_directory(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.rfind("models--", 0) != 0) continue;
        scan_tree(it->path() / "snapshots", "huggingface-cache");
      }
    };

    scan_hf_cache(DefaultHfCacheRoot());
    scan_tree(app_models_dir_, "project-models");

    std::sort(models.begin(), models.end(), [](const LocalModel& a, const LocalModel& b) {
      return a.name < b.name;
    });
    return models;
  }

 private:
  fs::path app_models_dir_;
};

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult RunCommandCapture(const std::vector<std::string>& argv) {
  const std::string command = JoinCommand(argv) + " 2>&1";
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) {
    return {-1, "failed to spawn command"};
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#ifdef _WIN32
  const int status = _pclose(pipe);
  return {status, output};
#else
  const int status = pclose(pipe);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
  return {exit_code, output};
#endif
}

enum class GpuBackend {
  None,
  Cpu,
  Cuda,
  Vulkan,
  Rocm,
  Sycl,
  Metal,
  OpenCL
};

struct GpuInfo {
  GpuBackend backend = GpuBackend::None;
  std::string name;
  std::string cuda_version;
};

class GpuDetector {
 public:
  static GpuInfo Detect() {
    GpuInfo info;
#ifdef _WIN32
    if (DetectNvidia(info)) return info;
    if (DetectAmdWindows(info)) return info;
    info.backend = GpuBackend::Cpu;
    return info;
#else
    if (DetectNvidia(info)) return info;
    if (DetectRocm(info)) return info;
    if (DetectMetal(info)) return info;
    info.backend = GpuBackend::Cpu;
    return info;
#endif
  }

 private:
  static bool DetectNvidia(GpuInfo& info) {
    auto probe = RunCommandCapture({"nvidia-smi", "--query-gpu=name", "--format=csv,noheader"});
    if (probe.exit_code == 0 && !Trim(probe.output).empty()) {
      info.name = Trim(probe.output);
      info.backend = GpuBackend::Cuda;
      auto ver_probe = RunCommandCapture({"nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"});
      if (ver_probe.exit_code == 0) {
        std::string ver = Trim(ver_probe.output);
        if (ver.rfind("13.", 0) == 0) {
          info.cuda_version = "13.1";
        } else if (ver.rfind("12.", 0) == 0) {
          info.cuda_version = "12.4";
        }
      }
      return true;
    }
    return false;
  }

  static bool DetectAmdWindows(GpuInfo& info) {
    auto probe = RunCommandCapture({"wmic", "path", "win32_VideoController", "get", "name", "/format:csv"});
    if (probe.exit_code == 0 && probe.output.find("Radeon") != std::string::npos) {
      info.name = "AMD GPU";
      info.backend = GpuBackend::Vulkan;
      return true;
    }
    return false;
  }

  static bool DetectRocm(GpuInfo& info) {
    auto probe = RunCommandCapture({"rocm-smi", "--showproductname"});
    if (probe.exit_code == 0 && !Trim(probe.output).empty()) {
      info.name = Trim(probe.output);
      info.backend = GpuBackend::Rocm;
      return true;
    }
    return false;
  }

  static bool DetectMetal(GpuInfo& info) {
    auto probe = RunCommandCapture({"system_profiler", "SPDisplaysDataType"});
    if (probe.exit_code == 0 && probe.output.find("Apple") != std::string::npos) {
      info.name = "Apple Silicon";
      info.backend = GpuBackend::Metal;
      return true;
    }
    return false;
  }
};

struct LlamaRelease {
  std::string tag;
  std::vector<std::string> assets;
};

class LlamaDownloader {
 public:
  explicit LlamaDownloader(HttpClient& http) : http_(http) {}

  std::optional<LlamaRelease> GetLatestRelease(std::string& error) const {
    auto resp = http_.Get("https://api.github.com/repos/ggml-org/llama.cpp/releases/latest");
    if (!resp.error.empty()) {
      error = resp.error;
      return std::nullopt;
    }
    if (resp.status != 200) {
      error = "HTTP " + std::to_string(resp.status);
      return std::nullopt;
    }
    auto payload = json::parse(resp.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      error = "invalid JSON";
      return std::nullopt;
    }
    LlamaRelease release;
    release.tag = payload.value("tag_name", "");
    for (const auto& asset : payload.value("assets", json::array())) {
      release.assets.push_back(asset.value("name", ""));
    }
    return release;
  }

  std::optional<std::string> FindAssetForPlatform(const LlamaRelease& release, const GpuInfo& gpu) const {
    std::vector<std::string> candidates;
#ifdef _WIN32
    if (gpu.backend == GpuBackend::Cuda) {
      std::string cuda = gpu.cuda_version.empty() ? "12.4" : gpu.cuda_version;
      candidates = {
          "llama-" + release.tag.substr(1) + "-bin-win-cuda-" + cuda + "-x64.zip",
          "llama-b8565-bin-win-cuda-" + cuda + "-x64.zip"};
    } else if (gpu.backend == GpuBackend::Vulkan || gpu.backend == GpuBackend::None) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-win-vulkan-x64.zip",
                    "llama-b8565-bin-win-vulkan-x64.zip"};
    } else if (gpu.backend == GpuBackend::Cpu) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-win-cpu-x64.zip",
                    "llama-b8565-bin-win-cpu-x64.zip"};
    } else if (gpu.backend == GpuBackend::Sycl) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-win-sycl-x64.zip",
                    "llama-b8565-bin-win-sycl-x64.zip"};
    }
#else
    auto sys_probe = RunCommandCapture({"uname", "-m"});
    bool is_arm = sys_probe.exit_code == 0 && Trim(sys_probe.output).find("arm") != std::string::npos;
    if (is_arm) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                    "llama-b8565-bin-macos-arm64.tar.gz"};
    } else if (gpu.backend == GpuBackend::Metal) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                    "llama-b8565-bin-macos-arm64.tar.gz"};
    } else if (gpu.backend == GpuBackend::Cuda) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                    "llama-b8565-bin-ubuntu-x64.tar.gz"};
    } else if (gpu.backend == GpuBackend::Vulkan) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-vulkan-x64.tar.gz",
                    "llama-b8565-bin-ubuntu-vulkan-x64.tar.gz"};
    } else if (gpu.backend == GpuBackend::Rocm) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-rocm-7.2-x64.tar.gz",
                    "llama-b8565-bin-ubuntu-rocm-7.2-x64.tar.gz"};
    } else {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                    "llama-b8565-bin-ubuntu-x64.tar.gz"};
    }
#endif
    if (candidates.empty()) {
      candidates = {"llama-" + release.tag.substr(1) + "-bin-win-cpu-x64.zip",
                    "llama-b8565-bin-win-cpu-x64.zip"};
    }
    for (const auto& candidate : candidates) {
      for (const auto& asset : release.assets) {
        if (asset == candidate) {
          return candidate;
        }
      }
    }
    return std::nullopt;
  }

  std::string GetDownloadUrl(const std::string& tag, const std::string& asset) const {
    return "https://github.com/ggml-org/llama.cpp/releases/download/" + tag + "/" + asset;
  }

  bool DownloadAndExtract(const std::string& url,
                          const fs::path& target_dir,
                          std::string& error,
                          const std::function<void(std::string)>& progress,
                          ProgressFn on_dl_progress = nullptr) {
    fs::path temp = fs::temp_directory_path() / "llama-download";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    fs::path archive = temp / "llama.zip";
    progress("Downloading...");
    if (!http_.DownloadToFile(url, archive, error, {}, std::move(on_dl_progress))) {
      return false;
    }
    progress("Extracting...");
    fs::create_directories(target_dir, ec);
#ifdef _WIN32
    std::string cmd = "powershell -Command \"Expand-Archive -Path '" + archive.string() + "' -DestinationPath '" + target_dir.string() + "' -Force\"";
#else
    std::string cmd = "tar -xzf " + ShellQuote(archive.string()) + " -C " + ShellQuote(target_dir.string());
#endif
    int result = system(cmd.c_str());
    if (result != 0) {
      error = "extraction failed";
      return false;
    }
    progress("Done!");
    return true;
  }

 private:
  HttpClient& http_;
};

class ProcessSupervisor {
 public:
  using LogFn = std::function<void(const std::string&)>;

  ~ProcessSupervisor() { Stop(); }

  bool Start(const std::vector<std::string>& argv, LogFn log_fn) {
    Stop();
    if (argv.empty()) return false;
    log_fn_ = std::move(log_fn);
    running_.store(false);

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
      return false;
    }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(read_pipe);
      CloseHandle(write_pipe);
      return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string command_line;
    for (size_t i = 0; i < argv.size(); ++i) {
      if (i) command_line += ' ';
      command_line += ShellQuote(argv[i]);
    }

    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    if (!CreateProcessA(nullptr,
                        mutable_command.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &si,
                        &pi)) {
      CloseHandle(read_pipe);
      CloseHandle(write_pipe);
      return false;
    }

    CloseHandle(write_pipe);
    process_info_ = pi;
    read_handle_ = read_pipe;
    running_.store(true);

    reader_thread_ = std::thread([this] {
      char buffer[4096];
      DWORD bytes_read = 0;
      while (running_.load()) {
        const BOOL ok = ReadFile(read_handle_, buffer, sizeof(buffer) - 1, &bytes_read, nullptr);
        if (!ok || bytes_read == 0) break;
        buffer[bytes_read] = '\0';
        if (log_fn_) log_fn_(std::string(buffer, bytes_read));
      }
    });
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
      return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]);
      close(pipefd[1]);
      return false;
    }

    if (pid == 0) {
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
      std::vector<char*> args;
      args.reserve(argv.size() + 1);
      for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
      args.push_back(nullptr);
      execvp(args[0], args.data());
      _exit(127);
    }

    close(pipefd[1]);
    pid_ = pid;
    read_fd_ = pipefd[0];
    running_.store(true);

    reader_thread_ = std::thread([this] {
      char buffer[4096];
      while (running_.load()) {
        const ssize_t n = read(read_fd_, buffer, sizeof(buffer));
        if (n <= 0) break;
        if (log_fn_) log_fn_(std::string(buffer, buffer + n));
      }
    });
#endif
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;

#ifdef _WIN32
    if (process_info_.hProcess) {
      TerminateProcess(process_info_.hProcess, 0);
      WaitForSingleObject(process_info_.hProcess, 5000);
    }
    if (read_handle_) {
      CloseHandle(read_handle_);
      read_handle_ = nullptr;
    }
    if (process_info_.hThread) {
      CloseHandle(process_info_.hThread);
      process_info_.hThread = nullptr;
    }
    if (process_info_.hProcess) {
      CloseHandle(process_info_.hProcess);
      process_info_.hProcess = nullptr;
    }
#else
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      int status = 0;
      waitpid(pid_, &status, 0);
      pid_ = -1;
    }
    if (read_fd_ >= 0) {
      close(read_fd_);
      read_fd_ = -1;
    }
#endif
    if (reader_thread_.joinable()) reader_thread_.join();
  }

  bool Running() const { return running_.load(); }

 private:
  std::atomic<bool> running_{false};
  LogFn log_fn_;
  std::thread reader_thread_;

#ifdef _WIN32
  HANDLE read_handle_ = nullptr;
  PROCESS_INFORMATION process_info_{};
#else
  pid_t pid_ = -1;
  int read_fd_ = -1;
#endif
};

class LlamaServerManager {
 public:
  explicit LlamaServerManager(ProcessSupervisor& process, HttpClient& http) : process_(process), http_(http) {}

  static std::optional<fs::path> FindBundledBinary(const fs::path& runtime_dir) {
#ifdef _WIN32
    const std::vector<std::string> names = {"llama-server.exe", "server.exe"};
#else
    const std::vector<std::string> names = {"llama-server", "server"};
#endif
    for (const auto& name : names) {
      const fs::path direct = runtime_dir / name;
      if (fs::exists(direct)) return direct;
      const fs::path bin_path = runtime_dir / "bin" / name;
      if (fs::exists(bin_path)) return bin_path;
    }

    if (fs::exists(runtime_dir)) {
      fs::directory_options options = fs::directory_options::skip_permission_denied;
      for (fs::recursive_directory_iterator it(runtime_dir, options), end; it != end; ++it) {
        std::error_code ec;
        if (!it->is_regular_file(ec)) continue;
        const auto filename = ToLower(it->path().filename().string());
        for (const auto& candidate : names) {
          if (filename == ToLower(candidate)) {
            return it->path();
          }
        }
      }
    }

#ifdef _WIN32
    const auto probe = RunCommandCapture({"llama-server.exe", "--version"});
    if (probe.exit_code == 0) return fs::path("llama-server.exe");
#else
    const auto probe = RunCommandCapture({"llama-server", "--version"});
    if (probe.exit_code == 0) return fs::path("llama-server");
#endif
    return std::nullopt;
  }

  bool StartWithLocalModel(const fs::path& binary,
                           const fs::path& model,
                           const std::string& host,
                           const std::string& port,
                           const std::string& extra_args,
                           const ProcessSupervisor::LogFn& log_fn,
                           std::string& command_preview) {
    std::vector<std::string> argv = {binary.string(), "-m", model.string(), "--host", host, "--port", port};
    const auto extra = SplitArgs(extra_args);
    argv.insert(argv.end(), extra.begin(), extra.end());
    command_preview = JoinCommand(argv);
    return process_.Start(argv, log_fn);
  }

  bool StartWithHfRepo(const fs::path& binary,
                       const std::string& repo,
                       const std::string& filename,
                       const std::string& host,
                       const std::string& port,
                       const std::string& extra_args,
                       const ProcessSupervisor::LogFn& log_fn,
                       std::string& command_preview) {
    std::vector<std::string> argv = {
        binary.string(), "--hf-repo", repo, "--hf-file", filename, "--host", host, "--port", port,
    };
    const auto extra = SplitArgs(extra_args);
    argv.insert(argv.end(), extra.begin(), extra.end());
    command_preview = JoinCommand(argv);
    return process_.Start(argv, log_fn);
  }

  void Stop() { process_.Stop(); }
  bool Running() const { return process_.Running(); }

  std::string Health(const std::string& host, const std::string& port) {
    const std::string url = "http://" + host + ":" + port + "/health";
    const auto response = http_.Get(url);
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
      return "unreachable";
    }
    return Trim(response.body).empty() ? "healthy" : Trim(response.body);
  }

 private:
  ProcessSupervisor& process_;
  HttpClient& http_;
};

struct SlashCommand {
  std::string name;
  std::string usage;
  std::string description;
};

class App {
 public:
  App()
      : screen_(ScreenInteractive::FullscreenAlternateScreen()),
        project_models_dir_(fs::current_path() / "models"),
        runtime_dir_(fs::current_path() / "runtime" / "llama.cpp"),
        scanner_(project_models_dir_),
        hf_client_(http_client_),
        server_manager_(process_, http_client_),
        downloader_(http_client_),
        source_toggle_entries_({"Local GGUF", "HF Repo/File"}),
        slash_commands_({
            {"/help", "/help", "Show available slash commands."},
            {"/search", "/search <query>", "Search Hugging Face repos."},
            {"/files", "/files", "List GGUF files for the selected repo."},
            {"/download", "/download", "Download the selected GGUF into models/."},
            {"/start", "/start", "Launch llama-server with the selected target."},
            {"/stop", "/stop", "Stop llama-server."},
            {"/health", "/health", "Check the /health endpoint."},
            {"/rescan", "/rescan", "Rescan local GGUFs and cache."},
            {"/refresh-binary", "/refresh-binary", "Rescan runtime/llama.cpp for llama-server."},
            {"/download-binary", "/download-binary", "Download llama.cpp for your GPU."},
            {"/yolo", "/yolo [on|off|toggle]", "Toggle confirmation-free mode."},
            {"/welcome", "/welcome", "Show the splash screen again."},
            {"/focus", "/focus <search|models|server|command>", "Move focus to a specific control."},
        }) {
    fs::create_directories(project_models_dir_);
    fs::create_directories(runtime_dir_);

    search_query_ = "Qwen GGUF";
    host_input_ = "127.0.0.1";
    port_input_ = "8080";
    extra_args_input_ = "-c 4096";

    repo_entries_ = {"(none)"};
    file_entries_ = {"(none)"};

    RefreshBinaryStatus();
    RefreshLocalModels();
    BuildUi();
    UpdateCommandSuggestions();

    AddLog("Forge-Point booted.");
    AddLog("Drop a prebuilt llama.cpp release inside runtime/llama.cpp/ and Forge-Point will auto-detect llama-server.");
  }

  void Run() { screen_.Loop(root_); }

 private:
  void PostToUi(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(ui_queue_mutex_);
      ui_queue_.push_back(std::move(fn));
    }
    screen_.PostEvent(Event::Custom);
  }

  void DrainUiQueue() {
    std::deque<std::function<void()>> batch;
    {
      std::lock_guard<std::mutex> lock(ui_queue_mutex_);
      batch.swap(ui_queue_);
    }
    for (auto& fn : batch) {
      fn();
    }
  }

  void CleanupFinishedWorkers() {
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [](const std::jthread& t) { return !t.joinable(); }),
        workers_.end());
  }

  void SpawnWorker(std::function<void()> fn) {
    CleanupFinishedWorkers();
    workers_.emplace_back([f = std::move(fn)] { f(); });
  }

  void AddLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::istringstream iss(line);
    std::string part;
    while (std::getline(iss, part)) {
      logs_.push_back(part);
    }
    while (logs_.size() > kMaxLogs) {
      logs_.erase(logs_.begin());
    }
    screen_.PostEvent(Event::Custom);
  }

  size_t AddTransfer(const std::string& label) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    transfers_.push_back({label, 0, 0, false, false});
    return transfers_.size() - 1;
  }

  void UpdateTransfer(size_t idx, uint64_t downloaded, uint64_t total) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    if (idx < transfers_.size()) {
      transfers_[idx].downloaded = downloaded;
      transfers_[idx].total = total;
    }
    screen_.PostEvent(Event::Custom);
  }

  void FinishTransfer(size_t idx, bool failed = false) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    if (idx < transfers_.size()) {
      transfers_[idx].done = true;
      transfers_[idx].failed = failed;
    }
    screen_.PostEvent(Event::Custom);
  }

  void CleanupDoneTransfers() {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    transfers_.erase(
        std::remove_if(transfers_.begin(), transfers_.end(),
                       [](const Transfer& t) { return t.done; }),
        transfers_.end());
  }

  void ShowWelcome() {
    show_welcome_ = true;
    AddLog("Welcome screen opened.");
  }

  void HideWelcome() {
    if (show_welcome_) {
      show_welcome_ = false;
      AddLog("Welcome screen dismissed.");
    }
  }

  void ToggleYolo() {
    yolo_mode_ = !yolo_mode_;
    AddLog(std::string("YOLO mode ") + (yolo_mode_ ? "enabled." : "disabled."));
  }

  void RefreshBinaryStatus() {
    gpu_info_ = GpuDetector::Detect();
    binary_path_ = LlamaServerManager::FindBundledBinary(runtime_dir_);
    if (binary_path_) {
      binary_status_ = "llama-server: " + binary_path_->string();
    } else {
      std::string gpu_name = gpu_info_.name.empty() ? "CPU" : gpu_info_.name;
      binary_status_ = "No llama-server. GPU: " + gpu_name;
    }
  }

  void DownloadLlamaCpp() {
    std::string error;
    AddLog("Fetching latest llama.cpp release...");
    auto release = downloader_.GetLatestRelease(error);
    if (!release) {
      AddLog("Failed to fetch release: " + error);
      return;
    }
    AddLog("Release: " + release->tag);
    auto asset = downloader_.FindAssetForPlatform(*release, gpu_info_);
    if (!asset) {
      AddLog("No compatible binary found for this platform.");
      return;
    }
    AddLog("Downloading: " + *asset);
    auto url = downloader_.GetDownloadUrl(release->tag, *asset);
    size_t tid = AddTransfer("llama.cpp: " + *asset);
    auto dl_progress = [this, tid](uint64_t dl, uint64_t total) {
      UpdateTransfer(tid, dl, total);
    };
    bool ok = downloader_.DownloadAndExtract(url, runtime_dir_, error, [this](const std::string& msg) {
      AddLog(msg);
    }, dl_progress);
    if (ok) {
      FinishTransfer(tid);
      AddLog("Download complete!");
      PostToUi([this] { RefreshBinaryStatus(); });
    } else {
      FinishTransfer(tid, true);
      AddLog("Download failed: " + error);
    }
  }

  void RefreshLocalModels() {
    local_models_ = scanner_.Scan();
    local_model_entries_.clear();
    for (const auto& model : local_models_) {
      local_model_entries_.push_back(model.name + " [" + HumanBytes(model.size) + "]");
    }
    if (local_model_entries_.empty()) {
      local_model_entries_.push_back("(none)");
    }
    if (local_selected_ >= static_cast<int>(local_model_entries_.size())) {
      local_selected_ = std::max(0, static_cast<int>(local_model_entries_.size()) - 1);
    }
    AddLog("Local scan complete. Found " + std::to_string(local_models_.size()) + " GGUF file(s).");
  }

  void SearchRepos() {
    std::string error;
    hf_repos_ = hf_client_.SearchRepos(search_query_, error);
    repo_entries_.clear();
    repo_files_.clear();
    file_entries_.clear();
    repo_selected_ = 0;
    file_selected_ = 0;
    active_repo_sha_.clear();

    if (!error.empty()) {
      AddLog("HF search failed: " + error);
      return;
    }

    for (const auto& repo : hf_repos_) {
      repo_entries_.push_back(repo.id + " (↓" + std::to_string(repo.downloads) + ", ♥" + std::to_string(repo.likes) + ")");
    }
    if (repo_entries_.empty()) {
      repo_entries_.push_back("(none)");
    }
    AddLog("HF search returned " + std::to_string(hf_repos_.size()) + " repos.");
  }

  void LoadRepoFiles() {
    if (hf_repos_.empty() || repo_selected_ < 0 || repo_selected_ >= static_cast<int>(hf_repos_.size())) {
      AddLog("Choose a repo first.");
      return;
    }
    std::string error;
    active_repo_sha_ = hf_repos_[repo_selected_].sha;
    repo_files_ = hf_client_.ListGgufFiles(hf_repos_[repo_selected_].id, active_repo_sha_, error);
    file_entries_.clear();
    file_selected_ = 0;

    if (!error.empty()) {
      AddLog("HF file listing failed: " + error);
      return;
    }

    for (const auto& file : repo_files_) {
      std::string label = file.filename;
      if (file.size > 0) label += " [" + HumanBytes(file.size) + "]";
      file_entries_.push_back(label);
    }
    if (file_entries_.empty()) {
      file_entries_.push_back("(none)");
    }
    AddLog("Loaded " + std::to_string(repo_files_.size()) + " GGUF file(s) from the selected repo.");
  }

  void DownloadSelectedFile() {
    if (hf_repos_.empty() || repo_files_.empty() || repo_selected_ >= static_cast<int>(hf_repos_.size()) ||
        file_selected_ >= static_cast<int>(repo_files_.size())) {
      AddLog("Choose a repo and GGUF file before downloading.");
      return;
    }

    const auto repo = hf_repos_[repo_selected_];
    const auto file = repo_files_[file_selected_];
    const fs::path target_dir = project_models_dir_ / ReplaceAll(repo.id, "/", "__");
    const fs::path target_file = target_dir / fs::path(file.filename).filename();

    RequestAction("Download GGUF",
                  "Download " + file.filename + " into\n" + target_file.string(),
                  true,
                  [this, repo, file, target_file] {
                    AddLog("Downloading " + file.filename + "...");
                    size_t tid = AddTransfer(file.filename);
                    SpawnWorker([this, url = file.download_url, target_file, tid] {
                      std::string error;
                      auto progress = [this, tid](uint64_t dl, uint64_t total) {
                        UpdateTransfer(tid, dl, total);
                      };
                      if (hf_client_.DownloadFile(url, target_file, error, progress)) {
                        FinishTransfer(tid);
                        AddLog("Download finished: " + target_file.string());
                        PostToUi([this] { RefreshLocalModels(); });
                      } else {
                        FinishTransfer(tid, true);
                        AddLog("Download failed: " + error);
                      }
                    });
                  });
  }

  void StartServer() {
    RefreshBinaryStatus();
    if (!binary_path_) {
      AddLog("Cannot start server: llama-server not found.");
      return;
    }

    RequestAction("Start llama-server", DescribeServerLaunch(), true, [this] {
      auto logger = [this](const std::string& chunk) { AddLog(chunk); };
      bool ok = false;
      if (source_mode_ == 0) {
        if (local_models_.empty() || local_selected_ >= static_cast<int>(local_models_.size())) {
          AddLog("Choose a local GGUF first.");
          return;
        }
        ok = server_manager_.StartWithLocalModel(*binary_path_,
                                                 local_models_[local_selected_].path,
                                                 host_input_,
                                                 port_input_,
                                                 extra_args_input_,
                                                 logger,
                                                 last_command_preview_);
      } else {
        if (hf_repos_.empty() || repo_files_.empty() || repo_selected_ >= static_cast<int>(hf_repos_.size()) ||
            file_selected_ >= static_cast<int>(repo_files_.size())) {
          AddLog("Choose a repo and GGUF file first.");
          return;
        }
        ok = server_manager_.StartWithHfRepo(*binary_path_,
                                             hf_repos_[repo_selected_].id,
                                             repo_files_[file_selected_].filename,
                                             host_input_,
                                             port_input_,
                                             extra_args_input_,
                                             logger,
                                             last_command_preview_);
      }
      if (ok) {
        AddLog("Server starting...");
        AddLog(last_command_preview_);
      } else {
        AddLog("Failed to launch llama-server.");
      }
    });
  }

  std::string DescribeServerLaunch() const {
    std::ostringstream oss;
    oss << "Host: " << host_input_ << '\n';
    oss << "Port: " << port_input_ << '\n';
    oss << "Args: " << extra_args_input_ << '\n';
    if (source_mode_ == 0 && !local_models_.empty() && local_selected_ < static_cast<int>(local_models_.size())) {
      oss << "Mode: Local GGUF\n";
      oss << "Model: " << local_models_[local_selected_].path.string();
    } else if (source_mode_ == 1 && !hf_repos_.empty() && !repo_files_.empty() &&
               repo_selected_ < static_cast<int>(hf_repos_.size()) && file_selected_ < static_cast<int>(repo_files_.size())) {
      oss << "Mode: HF Repo/File\n";
      oss << "Repo: " << hf_repos_[repo_selected_].id << '\n';
      oss << "File: " << repo_files_[file_selected_].filename;
    } else {
      oss << "Mode: (target not fully selected)";
    }
    return oss.str();
  }

  void StopServer() {
    RequestAction("Stop llama-server",
                  "Stop the managed llama-server process?",
                  true,
                  [this] {
                    if (!server_manager_.Running()) {
                      AddLog("Server is not running.");
                      return;
                    }
                    server_manager_.Stop();
                    AddLog("Server stopped.");
                  });
  }

  void CheckHealth() {
    health_status_ = server_manager_.Health(host_input_, port_input_);
    AddLog("Health: " + health_status_);
  }

  void RequestAction(std::string title, std::string detail, bool guarded, std::function<void()> action) {
    if (guarded && !yolo_mode_) {
      confirm_title_ = std::move(title);
      confirm_detail_ = std::move(detail);
      pending_confirm_action_ = std::move(action);
      show_confirm_ = true;
      return;
    }
    action();
  }

  void ConfirmAction() {
    if (!show_confirm_ || !pending_confirm_action_) return;
    show_confirm_ = false;
    auto action = std::move(pending_confirm_action_);
    pending_confirm_action_ = nullptr;
    action();
  }

  void CancelAction() {
    if (!show_confirm_) return;
    show_confirm_ = false;
    pending_confirm_action_ = nullptr;
    AddLog("Action cancelled.");
  }

  enum class Panel { Local = 0, Hub, Server, Command, COUNT };

  void CyclePanel(int delta) {
    const int count = static_cast<int>(Panel::COUNT);
    active_panel_ = static_cast<Panel>((static_cast<int>(active_panel_) + delta + count) % count);
    FocusActivePanel();
  }

  void FocusActivePanel() {
    switch (active_panel_) {
      case Panel::Local:   local_menu_->TakeFocus(); break;
      case Panel::Hub:     search_input_->TakeFocus(); break;
      case Panel::Server:  host_input_component_->TakeFocus(); break;
      case Panel::Command: command_input_component_->TakeFocus(); break;
      default: break;
    }
  }

  void BuildUi() {
    search_input_ = Input(&search_query_, "Qwen GGUF / org/model");
    host_input_component_ = Input(&host_input_, "127.0.0.1");
    port_input_component_ = Input(&port_input_, "8080");
    extra_args_component_ = Input(&extra_args_input_, "-c 4096");
    command_input_component_ = Input(&command_input_, "/help, /search, /start...");

    local_menu_ = Menu(&local_model_entries_, &local_selected_);
    repo_menu_ = Menu(&repo_entries_, &repo_selected_);
    file_menu_ = Menu(&file_entries_, &file_selected_);
    source_toggle_ = Toggle(&source_toggle_entries_, &source_mode_);

    auto local_panel = Container::Vertical({local_menu_, source_toggle_});
    auto hub_panel = Container::Vertical({search_input_, repo_menu_, file_menu_});
    auto server_panel = Container::Vertical({host_input_component_, port_input_component_, extra_args_component_});

    root_container_ = Container::Vertical({
        local_panel,
        hub_panel,
        server_panel,
        command_input_component_,
    });

    root_ = Renderer(root_container_, [this] {
      DrainUiQueue();
      Element body = show_welcome_ ? BuildWelcomeScreen() : BuildMainScreen();
      if (show_confirm_) {
        body = dbox({body, BuildConfirmOverlay()});
      }
      return body;
    });

    root_ = CatchEvent(root_, [this](Event event) {
      if (show_welcome_) {
        if (event == Event::Return || event == Event::Escape || event == Event::Character(' ')) {
          HideWelcome();
          active_panel_ = Panel::Command;
          FocusActivePanel();
          return true;
        }
        return true;
      }

      if (show_confirm_) {
        if (event == Event::Return || event == Event::Character('y') || event == Event::Character('Y')) {
          ConfirmAction();
          return true;
        }
        if (event == Event::Escape || event == Event::Character('n') || event == Event::Character('N')) {
          CancelAction();
          return true;
        }
        return true;
      }

      if (event == Event::Tab) {
        if (command_input_component_->Focused() && !command_input_.empty() && command_input_.front() == '/') {
          AcceptSuggestion();
          return true;
        }
        CyclePanel(1);
        return true;
      }

      if (event == Event::TabReverse) {
        CyclePanel(-1);
        return true;
      }

      // Ctrl+Y toggles YOLO
      if (event == Event::Special({25})) {
        ToggleYolo();
        return true;
      }

      if (event == Event::Character('q') || event == Event::CtrlC) {
        if (command_input_component_->Focused() || search_input_->Focused() ||
            host_input_component_->Focused() || port_input_component_->Focused() ||
            extra_args_component_->Focused()) {
          if (event == Event::Character('q')) return false;
        }
        screen_.ExitLoopClosure()();
        return true;
      }

      if (event == Event::Character('/')) {
        OpenCommandPalette();
        return true;
      }

      if (event == Event::Escape) {
        active_panel_ = Panel::Command;
        FocusActivePanel();
        return true;
      }

      // Context-sensitive Enter
      if (event == Event::Return) {
        if (command_input_component_->Focused()) {
          ExecuteCommandInput();
          return true;
        }
        if (search_input_->Focused()) {
          SearchRepos();
          return true;
        }
        if (repo_menu_->Focused()) {
          LoadRepoFiles();
          return true;
        }
        if (file_menu_->Focused()) {
          DownloadSelectedFile();
          return true;
        }
        if (local_menu_->Focused()) {
          if (!local_models_.empty() && local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
            source_mode_ = 0;
            AddLog("Selected: " + local_models_[local_selected_].name);
          }
          return true;
        }
      }

      // Arrow navigation within command palette
      if (command_input_component_->Focused()) {
        if (event == Event::ArrowDown) {
          MoveSuggestion(1);
          return true;
        }
        if (event == Event::ArrowUp) {
          MoveSuggestion(-1);
          return true;
        }
      }

      return false;
    });

    command_input_component_ = CatchEvent(command_input_component_, [this](Event event) {
      if (event.is_character()) {
        screen_.PostEvent(Event::Custom);
      }
      return false;
    });
  }

  void OpenCommandPalette() {
    if (command_input_.empty() || command_input_.front() != '/') command_input_ = "/";
    command_input_component_->TakeFocus();
    UpdateCommandSuggestions();
  }

  void MoveSuggestion(int delta) {
    UpdateCommandSuggestions();
    if (command_suggestions_.empty()) return;
    const int count = static_cast<int>(command_suggestions_.size());
    command_selected_ = (command_selected_ + delta + count) % count;
  }

  void AcceptSuggestion() {
    UpdateCommandSuggestions();
    if (command_suggestions_.empty()) return;
    const std::string replacement = command_suggestions_[command_selected_].usage;
    const auto first_space = command_input_.find(' ');
    if (first_space == std::string::npos) {
      command_input_ = replacement;
      if (replacement.find(' ') == std::string::npos) command_input_ += ' ';
    } else {
      command_input_ = replacement.substr(0, replacement.find(' ')) + command_input_.substr(first_space);
    }
    UpdateCommandSuggestions();
  }

  void UpdateCommandSuggestions() {
    command_suggestions_.clear();
    const std::string trimmed = Trim(command_input_);
    if (trimmed.empty() || trimmed.front() != '/') {
      command_selected_ = 0;
      return;
    }

    std::string prefix = trimmed;
    const auto space = trimmed.find(' ');
    if (space != std::string::npos) {
      prefix = trimmed.substr(0, space);
    }

    for (const auto& cmd : slash_commands_) {
      if (prefix == "/" || StartsWithIgnoreCase(cmd.name, prefix) || StartsWithIgnoreCase(cmd.usage, prefix)) {
        command_suggestions_.push_back(cmd);
      }
    }
    if (command_selected_ >= static_cast<int>(command_suggestions_.size())) command_selected_ = 0;
  }

  void ExecuteCommandInput() {
    const std::string raw = Trim(command_input_);
    if (raw.empty()) return;
    AddLog(std::string("$ ") + raw);
    command_input_.clear();
    UpdateCommandSuggestions();

    const auto words = SplitWords(raw);
    if (words.empty()) return;

    const std::string cmd = ToLower(words[0]);
    auto arg = [&](size_t index) -> std::string {
      return index < words.size() ? words[index] : std::string{};
    };

    if (cmd == "/help") {
      AddLog("Slash commands:");
      for (const auto& item : slash_commands_) {
        AddLog("  " + item.usage + " — " + item.description);
      }
      return;
    }
    if (cmd == "/search") {
      if (words.size() >= 2) {
        search_query_ = raw.substr(raw.find(' ') + 1);
      }
      SearchRepos();
      return;
    }
    if (cmd == "/files") {
      LoadRepoFiles();
      return;
    }
    if (cmd == "/download") {
      DownloadSelectedFile();
      return;
    }
    if (cmd == "/start") {
      StartServer();
      return;
    }
    if (cmd == "/stop") {
      StopServer();
      return;
    }
    if (cmd == "/health") {
      CheckHealth();
      return;
    }
    if (cmd == "/rescan") {
      RefreshLocalModels();
      return;
    }
    if (cmd == "/refresh-binary") {
      RefreshBinaryStatus();
      AddLog(binary_status_);
      return;
    }
    if (cmd == "/download-binary") {
      if (binary_path_) {
        AddLog("llama-server already present. Use /refresh-binary to rescan.");
      } else {
        AddLog("Starting download in background...");
        SpawnWorker([this] { DownloadLlamaCpp(); });
      }
      return;
    }
    if (cmd == "/welcome") {
      ShowWelcome();
      return;
    }
    if (cmd == "/yolo") {
      const std::string value = ToLower(arg(1));
      if (value == "on") {
        yolo_mode_ = true;
      } else if (value == "off") {
        yolo_mode_ = false;
      } else {
        ToggleYolo();
        return;
      }
      AddLog(std::string("YOLO mode ") + (yolo_mode_ ? "enabled." : "disabled."));
      return;
    }
    if (cmd == "/focus") {
      const std::string target = ToLower(arg(1));
      if (target == "search" || target == "hub") {
        active_panel_ = Panel::Hub;
      } else if (target == "models" || target == "local") {
        active_panel_ = Panel::Local;
      } else if (target == "server") {
        active_panel_ = Panel::Server;
      } else {
        active_panel_ = Panel::Command;
      }
      FocusActivePanel();
      AddLog("Focus moved to " + (target.empty() ? std::string("command") : target) + ".");
      return;
    }

    AddLog("Unknown command. Try /help.");
  }

  Element BuildMainScreen() {
    UpdateCommandSuggestions();

    std::vector<std::string> logs_copy;
    {
      std::lock_guard<std::mutex> lock(log_mutex_);
      logs_copy = logs_;
    }
    const size_t start = logs_copy.size() > 80 ? logs_copy.size() - 80 : 0;
    std::ostringstream log_stream;
    for (size_t i = start; i < logs_copy.size(); ++i) {
      log_stream << logs_copy[i];
      if (i + 1 < logs_copy.size()) log_stream << '\n';
    }

    Elements layout;
    layout.push_back(BuildHeader());
    layout.push_back(separator());
    layout.push_back(hbox({BuildLocalPanel() | flex, BuildHubPanel() | flex, BuildServerPanel() | flex}));
    layout.push_back(separator());

    auto transfers_el = BuildTransfers();
    if (transfers_el) {
      layout.push_back(*transfers_el);
      layout.push_back(separator());
    }

    layout.push_back(window(text("Logs"), paragraph(log_stream.str().empty() ? "(no logs yet)" : log_stream.str()) | frame | size(HEIGHT, GREATER_THAN, 12)));
    layout.push_back(separator());
    layout.push_back(BuildCommandPalette());
    layout.push_back(separator());
    layout.push_back(text("q quit · / commands · Tab cycle panels · Esc focus command · Ctrl+Y yolo · Enter act") | dim);

    return vbox(std::move(layout)) | border;
  }

  std::optional<Element> BuildTransfers() {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    std::vector<Transfer> active;
    for (const auto& t : transfers_) {
      if (!t.done) active.push_back(t);
    }
    if (active.empty()) return std::nullopt;

    Elements rows;
    for (const auto& t : active) {
      std::string status_text;
      if (t.total > 0) {
        double pct = static_cast<double>(t.downloaded) / static_cast<double>(t.total);
        status_text = HumanBytes(t.downloaded) + " / " + HumanBytes(t.total) +
                      " (" + std::to_string(static_cast<int>(pct * 100.0)) + "%)";
        rows.push_back(hbox({
            text(t.label + " ") | bold,
            gauge(static_cast<float>(pct)) | flex,
            text(" " + status_text),
        }));
      } else {
        status_text = HumanBytes(t.downloaded) + " downloaded...";
        rows.push_back(hbox({
            text(t.label + " ") | bold,
            spinner(4, static_cast<int>(t.downloaded / 65536) % 4),
            text(" " + status_text) | dim,
        }));
      }
    }
    return window(text("Transfers"), vbox(std::move(rows)));
  }

  Element BuildHeader() const {
    Element yolo_chip = text(yolo_mode_ ? " YOLO ON " : " YOLO OFF ") |
                        bold |
                        color(yolo_mode_ ? Color::Black : Color::White) |
                        bgcolor(yolo_mode_ ? Color::YellowLight : Color::GrayDark);

    Element server_chip = text(server_manager_.Running() ? " SERVER RUNNING " : " SERVER STOPPED ") |
                          bold |
                          color(server_manager_.Running() ? Color::Black : Color::White) |
                          bgcolor(server_manager_.Running() ? Color::GreenLight : Color::RedLight);

    return hbox({
        vbox({
            text(kAppTitle) | bold | color(Color::CyanLight),
            text(kTagline) | dim,
        }) | flex,
        hbox({server_chip, text(" "), yolo_chip}),
    });
  }

  Element BuildLocalPanel() const {
    std::string details = "No local model selected.";
    if (!local_models_.empty() && local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
      const auto& m = local_models_[local_selected_];
      details = m.path.string() + "\norigin: " + m.origin + "\nsize: " + HumanBytes(m.size);
    }

    bool active = (active_panel_ == Panel::Local);
    Element title = active ? (text("▸ Local GGUFs") | color(Color::CyanLight)) : text("Local GGUFs");

    return window(title,
                  vbox({
                      hbox({text("HF cache: "), text(scanner_.CacheRoot().string()) | dim}) | flex,
                      separator(),
                      local_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 12),
                      separator(),
                      paragraph(details),
                      separator(),
                      text("/rescan · /download-binary · Enter select") | dim,
                  }));
  }

  Element BuildHubPanel() const {
    std::string repo_detail = "No repo selected.";
    if (!hf_repos_.empty() && repo_selected_ >= 0 && repo_selected_ < static_cast<int>(hf_repos_.size())) {
      const auto& repo = hf_repos_[repo_selected_];
      repo_detail = repo.id + "\ndownloads: " + std::to_string(repo.downloads) +
                    "\nlikes: " + std::to_string(repo.likes) +
                    "\nsha: " + (repo.sha.empty() ? "(unknown)" : repo.sha);
    }

    std::string file_detail = "No GGUF file selected.";
    if (!repo_files_.empty() && file_selected_ >= 0 && file_selected_ < static_cast<int>(repo_files_.size())) {
      const auto& file = repo_files_[file_selected_];
      file_detail = file.filename + "\nsize: " + HumanBytes(file.size) + "\n" + file.download_url;
    }

    bool active = (active_panel_ == Panel::Hub);
    Element title = active ? (text("▸ Hugging Face") | color(Color::CyanLight)) : text("Hugging Face");

    return window(title,
                  vbox({
                      search_input_->Render(),
                      separator(),
                      text("Repos (Enter → list files)") | bold,
                      repo_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                      separator(),
                      paragraph(repo_detail),
                      separator(),
                      text("GGUF files (Enter → download)") | bold,
                      file_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                      separator(),
                      paragraph(file_detail),
                      separator(),
                      text("/search · /files · /download") | dim,
                  }));
  }

  Element BuildServerPanel() const {
    std::string target = source_mode_ == 0 ? "Local GGUF" : "HF Repo/File";
    if (source_mode_ == 0 && !local_models_.empty() && local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
      target += "\n" + local_models_[local_selected_].path.string();
    }
    if (source_mode_ == 1 && !hf_repos_.empty() && !repo_files_.empty() &&
        repo_selected_ >= 0 && repo_selected_ < static_cast<int>(hf_repos_.size()) &&
        file_selected_ >= 0 && file_selected_ < static_cast<int>(repo_files_.size())) {
      target += "\nrepo: " + hf_repos_[repo_selected_].id + "\nfile: " + repo_files_[file_selected_].filename;
    }

    bool active = (active_panel_ == Panel::Server);
    Element title = active ? (text("▸ Server") | color(Color::CyanLight)) : text("Server");

    return window(title,
                  vbox({
                      text(binary_status_),
                      separator(),
                      source_toggle_->Render(),
                      separator(),
                      text("Target") | bold,
                      paragraph(target),
                      separator(),
                      text("Host"), host_input_component_->Render(),
                      text("Port"), port_input_component_->Render(),
                      text("Extra args"), extra_args_component_->Render(),
                      separator(),
                      text("Health: " + health_status_),
                      separator(),
                      text("/start · /stop · /health") | dim,
                  }));
  }

  Element BuildCommandPalette() {
    Elements suggestion_rows;
    if (command_input_.empty()) {
      suggestion_rows.push_back(text("Type / to open commands." ) | dim);
    } else if (command_suggestions_.empty()) {
      suggestion_rows.push_back(text("No matching slash commands.") | dim);
    } else {
      for (size_t i = 0; i < command_suggestions_.size() && i < 6; ++i) {
        const auto& cmd = command_suggestions_[i];
        Element row = hbox({text(cmd.usage) | bold, text("  "), text(cmd.description) | dim});
        if (static_cast<int>(i) == command_selected_) {
          row = row | bgcolor(Color::Blue) | color(Color::White);
        }
        suggestion_rows.push_back(row);
      }
    }

    return window(text("Slash command palette"),
                  vbox({
                      command_input_component_->Render(),
                      separator(),
                      vbox(std::move(suggestion_rows)),
                  }));
  }

  Element BuildConfirmOverlay() const {
    return vbox({
               filler(),
               hbox({
                   filler(),
                   window(text(confirm_title_),
                          vbox({
                              paragraph(confirm_detail_),
                              separator(),
                              text(yolo_mode_ ? "YOLO mode is on." : "Press Enter/Y to confirm, Esc/N to cancel.") | dim,
                          })) | size(WIDTH, LESS_THAN, 72),
                   filler(),
               }),
               filler(),
           }) |
           bgcolor(Color::Black);
  }

  Element BuildWelcomeScreen() const {
    auto line = [](const std::string& text_value, Color c) {
      return text(text_value) | color(c) | bold | center;
    };

    return vbox({
               filler(),
               line(R"(  ______                    ____        _       _   )", Color::BlueLight),
               line(R"( / ____/___  _________ ____/ __ )____  (_)___  / |_ )", Color::CyanLight),
               line(R"(/ /_  / __ \/ ___/ __ `/ _  / __  / __ \/ / __ \/ __/)", Color::GreenLight),
               line(R"(/ __/ / /_/ / /  / /_/ /  __/ /_/ / /_/ / / / / / /_ )", Color::YellowLight),
               line(R"(/_/    \____/_/   \__, /\___/_____/\____/_/_/ /_/\__/ )", Color::MagentaLight),
               line(R"(                 /____/                                  )", Color::RedLight),
               separator(),
               text("Forge-Point") | bold | center | color(Color::White),
               text("A terminal cockpit for GGUF discovery, download, and llama.cpp server control.") | center | dim,
               separator(),
               paragraphAlignCenter("Press Enter to enter the cockpit.") |
                   size(WIDTH, LESS_THAN, 80),
               separator(),
               text("/ opens the command palette · Tab cycles panels · Enter acts") | center | color(Color::CyanLight),
               text("Ctrl+Y toggles YOLO mode · Esc returns to command palette") | center | dim,
               filler(),
           }) |
           border;
  }

  ScreenInteractive screen_;
  fs::path project_models_dir_;
  fs::path runtime_dir_;

  HttpClient http_client_;
  HfCacheScanner scanner_;
  HfClient hf_client_;
  ProcessSupervisor process_;
  LlamaServerManager server_manager_;
  LlamaDownloader downloader_;
  GpuInfo gpu_info_;

  std::optional<fs::path> binary_path_;
  std::string binary_status_ = "unknown";
  std::string health_status_ = "not checked";
  std::string last_command_preview_;
  std::string active_repo_sha_;

  bool yolo_mode_ = false;
  bool show_welcome_ = true;
  bool show_confirm_ = false;

  std::string confirm_title_;
  std::string confirm_detail_;
  std::function<void()> pending_confirm_action_;

  std::string search_query_;
  std::string host_input_;
  std::string port_input_;
  std::string extra_args_input_;
  std::string command_input_;

  std::vector<LocalModel> local_models_;
  std::vector<HfRepo> hf_repos_;
  std::vector<HfFile> repo_files_;
  std::vector<std::jthread> workers_;

  std::vector<std::string> local_model_entries_;
  std::vector<std::string> repo_entries_;
  std::vector<std::string> file_entries_;
  std::vector<std::string> source_toggle_entries_;
  std::vector<SlashCommand> slash_commands_;
  std::vector<SlashCommand> command_suggestions_;

  int local_selected_ = 0;
  int repo_selected_ = 0;
  int file_selected_ = 0;
  int source_mode_ = 0;
  int command_selected_ = 0;

  Panel active_panel_ = Panel::Command;

  Component root_container_;
  Component root_;
  Component search_input_;
  Component host_input_component_;
  Component port_input_component_;
  Component extra_args_component_;
  Component command_input_component_;
  Component local_menu_;
  Component repo_menu_;
  Component file_menu_;
  Component source_toggle_;

  mutable std::mutex log_mutex_;
  std::vector<std::string> logs_;

  std::mutex ui_queue_mutex_;
  std::deque<std::function<void()>> ui_queue_;

  struct Transfer {
    std::string label;
    uint64_t downloaded = 0;
    uint64_t total = 0;
    bool done = false;
    bool failed = false;
  };

  mutable std::mutex transfers_mutex_;
  std::vector<Transfer> transfers_;
};

}  // namespace

int main() {
  try {
    App app;
    app.Run();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "Forge-Point crashed: %s\n", ex.what());
    return 1;
  }
}
