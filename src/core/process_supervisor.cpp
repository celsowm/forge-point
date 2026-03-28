#include "core/process_supervisor.hpp"

#include "core/string_utils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace forge {

struct ProcessSupervisor::Impl {
#ifdef _WIN32
  HANDLE read_handle = nullptr;
  PROCESS_INFORMATION process_info{};
#else
  pid_t pid = -1;
  int read_fd = -1;
#endif
};

ProcessSupervisor::ProcessSupervisor() : impl_(std::make_unique<Impl>()) {}
ProcessSupervisor::~ProcessSupervisor() { Stop(); }

bool ProcessSupervisor::Start(const std::vector<std::string>& argv, LogFn log_fn) {
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
    command_line += util::ShellQuote(argv[i]);
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
  impl_->process_info = pi;
  impl_->read_handle = read_pipe;
  running_.store(true);

  reader_thread_ = std::thread([this] {
    char buffer[4096];
    DWORD bytes_read = 0;
    while (running_.load()) {
      const BOOL ok = ReadFile(impl_->read_handle, buffer, sizeof(buffer) - 1, &bytes_read, nullptr);
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
  impl_->pid = pid;
  impl_->read_fd = pipefd[0];
  running_.store(true);

  reader_thread_ = std::thread([this] {
    char buffer[4096];
    while (running_.load()) {
      const ssize_t n = read(impl_->read_fd, buffer, sizeof(buffer));
      if (n <= 0) break;
      if (log_fn_) log_fn_(std::string(buffer, buffer + n));
    }
  });
#endif
  return true;
}

void ProcessSupervisor::Stop() {
  if (!running_.exchange(false)) return;

#ifdef _WIN32
  if (impl_->process_info.hProcess) {
    TerminateProcess(impl_->process_info.hProcess, 0);
    WaitForSingleObject(impl_->process_info.hProcess, 5000);
  }
  if (impl_->read_handle) {
    CloseHandle(impl_->read_handle);
    impl_->read_handle = nullptr;
  }
  if (impl_->process_info.hThread) {
    CloseHandle(impl_->process_info.hThread);
    impl_->process_info.hThread = nullptr;
  }
  if (impl_->process_info.hProcess) {
    CloseHandle(impl_->process_info.hProcess);
    impl_->process_info.hProcess = nullptr;
  }
#else
  if (impl_->pid > 0) {
    kill(impl_->pid, SIGTERM);
    int status = 0;
    waitpid(impl_->pid, &status, 0);
    impl_->pid = -1;
  }
  if (impl_->read_fd >= 0) {
    close(impl_->read_fd);
    impl_->read_fd = -1;
  }
#endif
  if (reader_thread_.joinable()) reader_thread_.join();
}

bool ProcessSupervisor::Running() const { return running_.load(); }

}  // namespace forge
