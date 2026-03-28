#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
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

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
    return fs::path(local_app_data) / "huggingface" / "hub";
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

class HttpClient {
 public:
  HttpClient() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~HttpClient() { curl_global_cleanup(); }

  HttpResponse Get(const std::string& url, const std::vector<std::string>& headers = {}) const {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
      response.error = "curl_easy_init failed";
      return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::WriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "forge-point/0.1");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    struct curl_slist* header_list = nullptr;
    for (const auto& h : headers) {
      header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      response.error = curl_easy_strerror(res);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return response;
  }

  bool DownloadToFile(const std::string& url,
                      const fs::path& target,
                      std::string& error,
                      const std::vector<std::string>& headers = {}) const {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    std::ofstream out(target, std::ios::binary);
    if (!out) {
      error = "failed to open destination file";
      return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
      error = "curl_easy_init failed";
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::WriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "forge-point/0.1");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    struct curl_slist* header_list = nullptr;
    for (const auto& h : headers) {
      header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    out.close();

    if (res != CURLE_OK) {
      error = curl_easy_strerror(res);
      fs::remove(target, ec);
      return false;
    }
    if (status < 200 || status >= 300) {
      error = "HTTP " + std::to_string(status);
      fs::remove(target, ec);
      return false;
    }
    return true;
  }

 private:
  static size_t WriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
  }

  static size_t WriteFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
  }
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

  bool DownloadFile(const std::string& url, const fs::path& target, std::string& error) {
    return http_.DownloadToFile(url, target, error, AuthHeaders());
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
      : screen_(ScreenInteractive::Fullscreen()),
        project_models_dir_(fs::current_path() / "models"),
        runtime_dir_(fs::current_path() / "runtime" / "llama.cpp"),
        scanner_(project_models_dir_),
        hf_client_(http_client_),
        server_manager_(process_, http_client_),
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
    binary_path_ = LlamaServerManager::FindBundledBinary(runtime_dir_);
    if (binary_path_) {
      binary_status_ = "llama-server: " + binary_path_->string();
    } else {
      binary_status_ = "No llama-server found in runtime/llama.cpp/ or PATH.";
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
                    workers_.emplace_back([this, url = file.download_url, target_file] {
                      std::string error;
                      if (hf_client_.DownloadFile(url, target_file, error)) {
                        AddLog("Download finished: " + target_file.string());
                        RefreshLocalModels();
                      } else {
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

  void BuildUi() {
    search_input_ = Input(&search_query_, "Qwen GGUF / org/model");
    host_input_component_ = Input(&host_input_, "127.0.0.1");
    port_input_component_ = Input(&port_input_, "8080");
    extra_args_component_ = Input(&extra_args_input_, "-c 4096");
    command_input_component_ = Input(&command_input_, "/help, /search, /start...");

    search_button_ = Button("Search repos", [this] { SearchRepos(); });
    list_files_button_ = Button("List GGUFs", [this] { LoadRepoFiles(); });
    download_button_ = Button("Download GGUF", [this] { DownloadSelectedFile(); });
    rescan_button_ = Button("Rescan", [this] { RefreshLocalModels(); });
    refresh_binary_button_ = Button("Refresh binary", [this] { RefreshBinaryStatus(); });
    start_button_ = Button("Start server", [this] { StartServer(); });
    stop_button_ = Button("Stop server", [this] { StopServer(); });
    health_button_ = Button("Check /health", [this] { CheckHealth(); });

    local_menu_ = Menu(&local_model_entries_, &local_selected_);
    repo_menu_ = Menu(&repo_entries_, &repo_selected_);
    file_menu_ = Menu(&file_entries_, &file_selected_);
    source_toggle_ = Toggle(&source_toggle_entries_, &source_mode_);

    root_container_ = Container::Vertical({
        search_input_,
        search_button_,
        list_files_button_,
        download_button_,
        rescan_button_,
        refresh_binary_button_,
        local_menu_,
        repo_menu_,
        file_menu_,
        source_toggle_,
        host_input_component_,
        port_input_component_,
        extra_args_component_,
        start_button_,
        stop_button_,
        health_button_,
        command_input_component_,
    });

    root_ = Renderer(root_container_, [this] {
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
          return true;
        }
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
      }

      if (event == Event::TabReverse) {
        ToggleYolo();
        return true;
      }

      if (event == Event::Character('q') || event == Event::CtrlC) {
        screen_.ExitLoopClosure()();
        return true;
      }

      if (!show_welcome_ && !show_confirm_ && event == Event::Character('/')) {
        OpenCommandPalette();
        return true;
      }

      if (command_input_component_->Focused()) {
        if (event == Event::ArrowDown) {
          MoveSuggestion(1);
          return true;
        }
        if (event == Event::ArrowUp) {
          MoveSuggestion(-1);
          return true;
        }
        if (event == Event::Tab) {
          AcceptSuggestion();
          return true;
        }
        if (event == Event::Return) {
          ExecuteCommandInput();
          return true;
        }
      }

      return false;
    });

    search_input_ = CatchEvent(search_input_, [this](Event event) {
      if (event == Event::Return) {
        SearchRepos();
        return true;
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
      if (target == "search") search_input_->TakeFocus();
      else if (target == "models") local_menu_->TakeFocus();
      else if (target == "server") host_input_component_->TakeFocus();
      else command_input_component_->TakeFocus();
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

    return vbox({
               BuildHeader(),
               separator(),
               hbox({BuildLocalPanel() | flex, BuildHubPanel() | flex, BuildServerPanel() | flex}),
               separator(),
               window(text("Logs"), paragraph(log_stream.str().empty() ? "(no logs yet)" : log_stream.str()) | frame | size(HEIGHT, GREATER_THAN, 12)),
               separator(),
               BuildCommandPalette(),
               separator(),
               text("q quit · / open command palette · Shift+Tab toggle YOLO") | dim,
           }) |
           border;
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

    return window(text("Local GGUFs"),
                  vbox({
                      hbox({text("HF cache: "), text(scanner_.CacheRoot().string()) | dim}) | flex,
                      separator(),
                      local_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 12),
                      separator(),
                      paragraph(details),
                      separator(),
                      hbox({rescan_button_->Render(), filler(), refresh_binary_button_->Render()}),
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

    return window(text("Hugging Face"),
                  vbox({
                      search_input_->Render(),
                      hbox({search_button_->Render(), filler(), list_files_button_->Render()}),
                      separator(),
                      text("Repos") | bold,
                      repo_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                      separator(),
                      paragraph(repo_detail),
                      separator(),
                      text("GGUF files") | bold,
                      file_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                      separator(),
                      paragraph(file_detail),
                      separator(),
                      download_button_->Render(),
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

    return window(text("Server"),
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
                      hbox({start_button_->Render(), filler(), stop_button_->Render()}),
                      health_button_->Render(),
                      separator(),
                      text("Health: " + health_status_),
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
               paragraphAlignCenter("Press Enter to enter the cockpit. Press Shift+Tab any time to toggle YOLO mode.") |
                   size(WIDTH, LESS_THAN, 80),
               separator(),
               text("/ opens the slash command palette") | center | color(Color::CyanLight),
               text("runtime/llama.cpp/ is where your prebuilt llama.cpp release lives") | center | dim,
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

  Component root_container_;
  Component root_;
  Component search_input_;
  Component host_input_component_;
  Component port_input_component_;
  Component extra_args_component_;
  Component command_input_component_;
  Component search_button_;
  Component list_files_button_;
  Component download_button_;
  Component rescan_button_;
  Component refresh_binary_button_;
  Component start_button_;
  Component stop_button_;
  Component health_button_;
  Component local_menu_;
  Component repo_menu_;
  Component file_menu_;
  Component source_toggle_;

  mutable std::mutex log_mutex_;
  std::vector<std::string> logs_;
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
