// SPDX-License-Identifier: MIT
#pragma once
#include "VirtualFile.h"
#include "ArchiveJobChecks.h"
#include "StagingSpace.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Listi {
inline void RunArchiveCompressor(const char *name, const std::string &working,
                                 std::vector<std::string> args, int outputFd,
                                 const TransferControl &control) {
  const auto require = [](bool ok, const std::string &message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  const std::string tool(name);
  control.Check();
  ArchiveJobChecks::FD scratch(open(working.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  auto checkSpace = [&] {
    require(scratch.n >= 0 && StagingSpaceAvailable(scratch.n, 0),
            "Not enough free space to preserve the " + tool + " staging reserve.");
  };
  checkSpace();
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  // Control the child locale and do not honor user/tool environment options.
  char locale[] = "LC_ALL=C";
  char *const env[] = {locale, nullptr};

  posix_spawn_file_actions_t actions;
  require(posix_spawn_file_actions_init(&actions) == 0,
          "Cannot set up " + tool + " writer process.");
  int error = posix_spawn_file_actions_addchdir(&actions, working.c_str());
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                             "/dev/null", O_RDONLY, 0);
  if (!error && outputFd > STDERR_FILENO)
    error = posix_spawn_file_actions_addclose(&actions, outputFd);
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                             "/dev/null", O_WRONLY, 0);
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                             "/dev/null", O_WRONLY, 0);
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, args.front().c_str(), &actions, nullptr,
                        argv.data(), env);
  posix_spawn_file_actions_destroy(&actions);
  require(!error,
          "Cannot start " + tool + " writer: " + std::string(strerror(error)));

  int status;
  bool childIsOwned = true;
  try {
    for (;;) {
      const auto result = waitpid(pid, &status, WNOHANG);
      if (result == pid)
        break;
      if (result < 0 && errno != EINTR) {
        childIsOwned = errno != ECHILD;
        throw std::runtime_error("Cannot wait for the " + tool + " writer.");
      }
      control.Check();
      checkSpace();
      usleep(20000);
    }
  } catch (...) {
    if (childIsOwned) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    throw;
  }
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "The " + tool + " writer could not create the archive.");
  checkSpace();
  control.Check();
}
} // namespace Listi
