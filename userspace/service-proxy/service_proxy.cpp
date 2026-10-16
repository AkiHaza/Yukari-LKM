#include "service_filter.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool readAll(int fd, std::string* output) {
  char buffer[8192];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count == 0) return true;
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    output->append(buffer, static_cast<size_t>(count));
  }
}

std::string callerProcess() {
  if (const char* value = std::getenv("YUKARI_CALLER_PROCESS")) return value;
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string value;
  std::getline(cmdline, value, '\0');
  return value;
}

uid_t callerUid() { return getuid(); }

int usage() {
  const char* message = "usage: service-proxy --exec /system/bin/service list [args...]\n";
  (void)write(STDERR_FILENO, message, std::strlen(message));
  return 64;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || std::strcmp(argv[1], "--exec") != 0 ||
      std::strcmp(argv[2], "/system/bin/service") != 0 ||
      std::strcmp(argv[3], "list") != 0) {
    return usage();
  }

  int output_pipe[2];
  if (pipe2(output_pipe, O_CLOEXEC) != 0) return 126;
  pid_t child = fork();
  if (child < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return 126;
  }
  if (child == 0) {
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
    close(output_pipe[0]);
    close(output_pipe[1]);
    // argv[2] is the real service executable and argv[3..] is its original
    // argument vector.  The helper itself never changes the command.
    std::vector<char*> service_argv;
    service_argv.reserve(static_cast<size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) service_argv.push_back(argv[i]);
    service_argv.push_back(nullptr);
    execv(argv[2], service_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  close(output_pipe[1]);
  std::string output;
  const bool read_ok = readAll(output_pipe[0], &output);
  close(output_pipe[0]);

  int status = 126;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (!read_ok || waited < 0) return 126;

  yukari::FilterConfig config = yukari::ServiceFilter::loadConfig();
  yukari::ServiceFilter filter(std::move(config));
  const std::string process = callerProcess();
  const std::string filtered =
      filter.matchesCaller(callerUid(), process) ? filter.filterServiceList(output) : output;
  const char* data = filtered.data();
  size_t remaining = filtered.size();
  while (remaining != 0) {
    const ssize_t written = write(STDOUT_FILENO, data, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return 126;
    }
    if (written == 0) return 126;
    data += written;
    remaining -= static_cast<size_t>(written);
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 126;
}

