#include "core/http_client.hpp"

#include "core/string_utils.hpp"

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace forge {

#ifdef _WIN32

struct HttpClient::Impl {
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

  static HttpResponse WinHttpGet(const std::string& url,
                                 const std::vector<std::string>& headers) {
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
        if (on_progress &&
            (now - last_report > std::chrono::milliseconds(250) || bytesAvailable == 0)) {
          on_progress(downloaded, total);
          last_report = now;
        }
      }
      bytesAvailable = 0;
    }
    if (on_progress) on_progress(downloaded, total);
    return true;
  }
};

#else

struct HttpClient::Impl {
  static HttpResponse WinHttpGet(const std::string& url,
                                 const std::vector<std::string>& headers) {
    HttpResponse response;
    std::string header_args;
    for (const auto& h : headers) {
      header_args += " -H " + util::ShellQuote(h);
    }
    std::string cmd = "curl -s -o - -w '\\n%{http_code}'" + header_args + " " +
                      util::ShellQuote(url);
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
    pclose(pipe);

    auto last_nl = all_output.rfind('\n');
    if (last_nl != std::string::npos) {
      std::string code_str = util::Trim(all_output.substr(last_nl + 1));
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
      header_args += " -H " + util::ShellQuote(h);
    }
    std::string cmd = "curl -s -L -o -" + header_args + " " + util::ShellQuote(url);
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
};

#endif

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::Get(const std::string& url,
                             const std::vector<std::string>& headers) const {
  return Impl::WinHttpGet(url, headers);
}

bool HttpClient::DownloadToFile(const std::string& url, const fs::path& target,
                                std::string& error,
                                const std::vector<std::string>& headers,
                                ProgressFn on_progress) const {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);

  const fs::path part_file = fs::path(target.string() + ".part");
  std::ofstream out(part_file, std::ios::binary);
  if (!out) {
    error = "failed to open destination file";
    return false;
  }

  bool ok = Impl::StreamDownload(url, headers, out, error, std::move(on_progress));
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

}  // namespace forge
