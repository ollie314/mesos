// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#ifdef __linux__
#include <sched.h>
#include <signal.h>
#endif // __linux__
#include <string.h>

#include <iostream>
#include <set>
#include <string>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/path.hpp>
#include <stout/unreachable.hpp>

#ifdef __linux__
#include "linux/capabilities.hpp"
#include "linux/fs.hpp"
#include "linux/ns.hpp"
#endif

#include "mesos/mesos.hpp"

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::set;
using std::string;
using std::vector;

#ifdef __linux__
using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;
#endif // __linux__

namespace mesos {
namespace internal {
namespace slave {

const string MesosContainerizerLaunch::NAME = "launch";


MesosContainerizerLaunch::Flags::Flags()
{
  add(&command,
      "command",
      "The command to execute.");

  add(&working_directory,
      "working_directory",
      "The working directory for the command. It has to be an absolute path \n"
      "w.r.t. the root filesystem used for the command.");

#ifndef __WINDOWS__
  add(&runtime_directory,
      "runtime_directory",
      "The runtime directory for the container (used for checkpointing)");

  add(&rootfs,
      "rootfs",
      "Absolute path to the container root filesystem. The command will be \n"
      "interpreted relative to this path");

  add(&user,
      "user",
      "The user to change to.");
#endif // __WINDOWS__

  add(&pipe_read,
      "pipe_read",
      "The read end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&pipe_write,
      "pipe_write",
      "The write end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&pre_exec_commands,
      "pre_exec_commands",
      "The additional preparation commands to execute before\n"
      "executing the command.");

#ifdef __linux__
  add(&unshare_namespace_mnt,
      "unshare_namespace_mnt",
      "Whether to launch the command in a new mount namespace.",
      false);

  add(&capabilities,
      "capabilities",
      "Capabilities of the command can use.");
#endif // __linux__
}


static Option<pid_t> containerPid = None();
static Option<string> containerStatusPath = None();
static Option<int> containerStatusFd = None();

static void exitWithSignal(int sig);
static void exitWithStatus(int status);


#ifndef __WINDOWS__
static void signalSafeWriteStatus(int status)
{
  const string statusString = std::to_string(status);

  Try<Nothing> write = os::write(
      containerStatusFd.get(),
      statusString);

  if (write.isError()) {
    os::write(STDERR_FILENO,
              "Failed to write container status '" +
              statusString + "': " + ::strerror(errno));
  }
}


// When launching the executor with an 'init' process, we need to
// forward all relevant signals to it. The functions below help to
// enable this forwarding.
static void signalHandler(int sig)
{
  // If we dn't yet have a container pid, we treat
  // receiving a signal like a failure and exit.
  if (containerPid.isNone()) {
    exitWithSignal(sig);
  }

  // Otherwise we simply forward the signal to `containerPid`. We
  // purposefully ignore the error here since we have to remain async
  // signal safe. The only possible error scenario relevant to us is
  // ESRCH, but if that happens that means our pid is already gone and
  // the process will exit soon. So we are safe.
  os::kill(containerPid.get(), sig);
}


static Try<Nothing> installSignalHandlers()
{
  // Install handlers for all standard POSIX signals
  // (i.e. any signal less than `NSIG`).
  for (int i = 1; i < NSIG; i++) {
    // We don't want to forward the SIGCHLD signal, nor do we want to
    // handle it ourselves because we reap all children inline in the
    // `execute` function.
    if (i == SIGCHLD) {
      continue;
    }

    // We can't catch or ignore these signals, so we shouldn't try
    // to register a handler for them.
    if (i == SIGKILL || i == SIGSTOP) {
      continue;
    }

    // The NSIG constant is used to determine the number of signals
    // available on a system. However, Darwin, Linux, and BSD differ
    // on their interpretation of of the value of NSIG. Linux, for
    // example, sets it to 65, where Darwin sets it to 32. The reason
    // for the discrepency is that Linux includes the real-time
    // signals in this count, where Darwin does not. However, even on
    // linux, we are not able to arbitrarily install signal handlers
    // for all the real-time signals -- they must have not been
    // registered with the system first. For this reason, we
    // standardize on verifying the installation of handlers for
    // signals 1-31 (since these are defined in the POSIX standard),
    // but we continue to attempt to install handlers up to the value
    // of NSIG without verification.
    const int posixLimit = 32;
    if (os::signals::install(i, signalHandler) != 0 && i < posixLimit) {
      return ErrnoError("Unable to register signal"
                        " '" + stringify(strsignal(i)) + "'");
    }
  }

  return Nothing();
}
#endif // __WINDOWS__


static void exitWithSignal(int sig)
{
#ifndef __WINDOWS__
  if (containerStatusFd.isSome()) {
    signalSafeWriteStatus(W_EXITCODE(0, sig));
    os::close(containerStatusFd.get());
  }
#endif // __WINDOWS__
  ::_exit(EXIT_FAILURE);
}


static void exitWithStatus(int status)
{
#ifndef __WINDOWS__
  if (containerStatusFd.isSome()) {
    signalSafeWriteStatus(W_EXITCODE(status, 0));
    os::close(containerStatusFd.get());
  }
#endif // __WINDOWS__
  ::_exit(status);
}


int MesosContainerizerLaunch::execute()
{
#ifndef __WINDOWS__
  // The existence of the `runtime_directory` flag implies that we
  // want to checkpoint the container's status upon exit.
  if (flags.runtime_directory.isSome()) {
    containerStatusPath = path::join(
        flags.runtime_directory.get(),
        containerizer::paths::STATUS_FILE);

    Try<int> open = os::open(
        containerStatusPath.get(),
        O_WRONLY | O_CREAT | O_CLOEXEC,
        S_IRUSR | S_IWUSR);

    if (open.isError()) {
      cerr << "Failed to open file for writing the container status"
           << " '" << containerStatusPath.get() << "':"
           << " " << open.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    containerStatusFd = open.get();
  }

  // We need a signal fence here to ensure that `containerStatusFd` is
  // actually written to memory and not just to a temporary register.
  // Without this, it's possible that the signal handler we are about
  // to install would never see the correct value since there's no
  // guarantee that it is written to memory until this function
  // completes (which won't happen for a really long time because we
  // do a blocking `waitpid()` below).
  std::atomic_signal_fence(std::memory_order_relaxed);

  // Install signal handlers for all incoming signals.
  Try<Nothing> signals = installSignalHandlers();
  if (signals.isError()) {
    cerr << "Failed to install signal handlers: " << signals.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }
#endif // __WINDOWS__

  // Check command line flags.
  if (flags.command.isNone()) {
    cerr << "Flag --command is not specified" << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  bool controlPipeSpecified =
    flags.pipe_read.isSome() && flags.pipe_write.isSome();

  if ((flags.pipe_read.isSome() && flags.pipe_write.isNone()) ||
      (flags.pipe_read.isNone() && flags.pipe_write.isSome())) {
    cerr << "Flag --pipe_read and --pipe_write should either be "
         << "both set or both not set" << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  // Parse the command.
  Try<CommandInfo> command =
    ::protobuf::parse<CommandInfo>(flags.command.get());

  if (command.isError()) {
    cerr << "Failed to parse the command: " << command.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  // Validate the command.
  if (command.get().shell()) {
    if (!command.get().has_value()) {
      cerr << "Shell command is not specified" << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  } else {
    if (!command.get().has_value()) {
      cerr << "Executable path is not specified" << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  if (controlPipeSpecified) {
    int pipe[2] = { flags.pipe_read.get(), flags.pipe_write.get() };

    // NOTE: On windows we need to pass `HANDLE`s between processes,
    // as file descriptors are not unique across processes. Here we
    // convert back from from the `HANDLE`s we receive to fds that can
    // be used in os-agnostic code.
#ifdef __WINDOWS__
    pipe[0] = os::handle_to_fd(pipe[0], _O_RDONLY | _O_TEXT);
    pipe[1] = os::handle_to_fd(pipe[1], _O_TEXT);
#endif // __WINDOWS__

    Try<Nothing> close = os::close(pipe[1]);
    if (close.isError()) {
      cerr << "Failed to close pipe[1]: " << close.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // Do a blocking read on the pipe until the parent signals us to continue.
    char dummy;
    ssize_t length;
    while ((length = os::read(pipe[0], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);

    if (length != sizeof(dummy)) {
      // There's a reasonable probability this will occur during
      // agent restarts across a large/busy cluster.
      cerr << "Failed to synchronize with agent "
           << "(it's probably exited)" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    close = os::close(pipe[0]);
    if (close.isError()) {
      cerr << "Failed to close pipe[0]: " << close.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

#ifdef __linux__
  if (flags.unshare_namespace_mnt) {
    if (unshare(CLONE_NEWNS) != 0) {
      cerr << "Failed to unshare mount namespace: "
           << os::strerror(errno) << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __linux__

  // Run additional preparation commands. These are run as the same
  // user and with the environment as the agent.
  if (flags.pre_exec_commands.isSome()) {
    // TODO(jieyu): Use JSON::Array if we have generic parse support.
    JSON::Array array = flags.pre_exec_commands.get();
    foreach (const JSON::Value& value, array.values) {
      if (!value.is<JSON::Object>()) {
        cerr << "Invalid JSON format for flag --commands" << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      Try<CommandInfo> parse = ::protobuf::parse<CommandInfo>(value);
      if (parse.isError()) {
        cerr << "Failed to parse a preparation command: "
             << parse.error() << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      if (!parse.get().has_value()) {
        cerr << "The 'value' of a preparation command is not specified" << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      cout << "Executing pre-exec command '" << value << "'" << endl;

      int status = 0;

      if (parse->shell()) {
        // Execute the command using the system shell.
        status = os::system(parse->value());
      } else {
        // Directly spawn all non-shell commands to prohibit users
        // from injecting arbitrary shell commands in the arguments.
        vector<string> args;
        foreach (const string& arg, parse->arguments()) {
          args.push_back(arg);
        }

        status = os::spawn(parse->value(), args);
      }

      if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        cerr << "Failed to execute pre-exec command '" << value << "'" << endl;
        exitWithStatus(EXIT_FAILURE);
      }
    }
  }

#ifndef __WINDOWS__
  // NOTE: If 'flags.user' is set, we will get the uid, gid, and the
  // supplementary group ids associated with the specified user before
  // changing the filesystem root. This is because after changing the
  // filesystem root, the current process might no longer have access
  // to /etc/passwd and /etc/group on the host.
  Option<uid_t> uid;
  Option<gid_t> gid;
  vector<gid_t> gids;

  // TODO(gilbert): For the case container user exists, support
  // framework/task/default user -> container user mapping once
  // user namespace and container capabilities is available for
  // mesos container.

  if (flags.user.isSome()) {
    Result<uid_t> _uid = os::getuid(flags.user.get());
    if (!_uid.isSome()) {
      cerr << "Failed to get the uid of user '" << flags.user.get() << "': "
           << (_uid.isError() ? _uid.error() : "not found") << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // No need to change user/groups if the specified user is the same
    // as that of the current process.
    if (_uid.get() != os::getuid().get()) {
      Result<gid_t> _gid = os::getgid(flags.user.get());
      if (!_gid.isSome()) {
        cerr << "Failed to get the gid of user '" << flags.user.get() << "': "
             << (_gid.isError() ? _gid.error() : "not found") << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      Try<vector<gid_t>> _gids = os::getgrouplist(flags.user.get());
      if (_gids.isError()) {
        cerr << "Failed to get the supplementary gids of user '"
             << flags.user.get() << "': "
             << (_gids.isError() ? _gids.error() : "not found") << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      uid = _uid.get();
      gid = _gid.get();
      gids = _gids.get();
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  // Initialize capabilities support if necessary.
  Try<Capabilities> capabilitiesManager = Error("Not initialized");

  if (flags.capabilities.isSome()) {
    capabilitiesManager = Capabilities::create();
    if (capabilitiesManager.isError()) {
      cerr << "Failed to initialize capabilities support: "
           << capabilitiesManager.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // Prevent clearing of capabilities on `setuid`.
    if (uid.isSome()) {
      Try<Nothing> keepCaps = capabilitiesManager->setKeepCaps();
      if (keepCaps.isError()) {
        cerr << "Failed to set process control for keeping capabilities "
             << "on potential uid change: " << keepCaps.error() << endl;
        exitWithStatus(EXIT_FAILURE);
      }
    }
  }
#endif // __linux__

#ifdef __WINDOWS__
  // Not supported on Windows.
  const Option<string> rootfs = None();
#else
  const Option<string> rootfs = flags.rootfs;
#endif // __WINDOWS__

  // Change root to a new root, if provided.
  if (rootfs.isSome()) {
    cout << "Changing root to " << rootfs.get() << endl;

    // Verify that rootfs is an absolute path.
    Result<string> realpath = os::realpath(rootfs.get());
    if (realpath.isError()) {
      cerr << "Failed to determine if rootfs is an absolute path: "
           << realpath.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    } else if (realpath.isNone()) {
      cerr << "Rootfs path does not exist" << endl;
      exitWithStatus(EXIT_FAILURE);
    } else if (realpath.get() != rootfs.get()) {
      cerr << "Rootfs path is not an absolute path" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

#ifdef __linux__
    Try<Nothing> chroot = fs::chroot::enter(rootfs.get());
#elif defined(__WINDOWS__)
    Try<Nothing> chroot = Error("`chroot` not supported on Windows");
#else // For any other platform we'll just use POSIX chroot.
    Try<Nothing> chroot = os::chroot(rootfs.get());
#endif // __linux__
    if (chroot.isError()) {
      cerr << "Failed to enter chroot '" << rootfs.get()
           << "': " << chroot.error();
      exitWithStatus(EXIT_FAILURE);
    }
  }

  // Change user if provided. Note that we do that after executing the
  // preparation commands so that those commands will be run with the
  // same privilege as the mesos-agent.
#ifndef __WINDOWS__
  if (uid.isSome()) {
    Try<Nothing> setgid = os::setgid(gid.get());
    if (setgid.isError()) {
      cerr << "Failed to set gid to " << gid.get()
           << ": " << setgid.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<Nothing> setgroups = os::setgroups(gids, uid);
    if (setgroups.isError()) {
      cerr << "Failed to set supplementary gids: "
           << setgroups.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<Nothing> setuid = os::setuid(uid.get());
    if (setuid.isError()) {
      cerr << "Failed to set uid to " << uid.get()
           << ": " << setuid.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  if (flags.capabilities.isSome()) {
    Try<CapabilityInfo> requestedCapabilities =
      ::protobuf::parse<CapabilityInfo>(flags.capabilities.get());

    if (requestedCapabilities.isError()) {
      cerr << "Failed to parse capabilities: "
           << requestedCapabilities.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<ProcessCapabilities> capabilities = capabilitiesManager->get();
    if (capabilities.isError()) {
      cerr << "Failed to get capabilities for the current process: "
           << capabilities.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // After 'setuid', 'effective' set is cleared. Since `SETPCAP` is
    // required in the `effective` set of a process to change the
    // bounding set, we need to restore it first.
    capabilities->add(capabilities::EFFECTIVE, capabilities::SETPCAP);

    Try<Nothing> setPcap = capabilitiesManager->set(capabilities.get());
    if (setPcap.isError()) {
      cerr << "Failed to add SETPCAP to the effective set: "
           << setPcap.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // Set up requested capabilities.
    set<Capability> target = capabilities::convert(requestedCapabilities.get());

    capabilities->set(capabilities::EFFECTIVE, target);
    capabilities->set(capabilities::PERMITTED, target);
    capabilities->set(capabilities::INHERITABLE, target);
    capabilities->set(capabilities::BOUNDING, target);

    Try<Nothing> set = capabilitiesManager->set(capabilities.get());
    if (set.isError()) {
      cerr << "Failed to set process capabilities: " << set.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __linux__

  if (flags.working_directory.isSome()) {
    Try<Nothing> chdir = os::chdir(flags.working_directory.get());
    if (chdir.isError()) {
      cerr << "Failed to chdir into current working directory "
           << "'" << flags.working_directory.get() << "': "
           << chdir.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  // Relay the environment variables.
  // TODO(jieyu): Consider using a clean environment.

#ifndef __WINDOWS__
  // If we have `containerStatusFd` set, then we need to fork-exec the
  // command we are launching and checkpoint its status on exit. We
  // use fork-exec directly (as opposed to `process::subprocess()`) to
  // avoid intializing libprocess for this simple helper binary.
  //
  // TODO(klueska): Once we move the majority of `process::subprocess()`
  // into stout, update the code below to use it.
  if (containerStatusFd.isSome()) {
    pid_t pid = ::fork();

    if (pid == -1) {
      cerr << "Failed to fork() the command: " << os::strerror(errno) << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // If we are the parent...
    if (pid > 0) {
      // Set the global `containerPid` variable to enable signal forwarding.
      //
      // NOTE: We need a signal fence here to ensure that `containerPid`
      // is actually written to memory and not just to a temporary register.
      // Without this, it's possible that the signal handler would
      // never notice the change since there's no guarantee that it is
      // written out to memory until this function completes (which
      // won't happen until it's too late because we loop inside a
      // blocking `waitpid()` call below).
      containerPid = pid;
      std::atomic_signal_fence(std::memory_order_relaxed);

      // Wait for the newly created process to finish.
      int status = 0;
      Result<pid_t> waitpid = None();

      // Reap all decendants, but only continue once we reap the
      // process we just launched.
      while (true) {
        waitpid = os::waitpid(-1, &status, 0);

        if (waitpid.isError()) {
          // If the error was an EINTR, we were interrupted by a
          // signal and should just call `waitpid()` over again.
          if (errno == EINTR) {
            continue;
          }
          cerr << "Failed to os::waitpid(): " << waitpid.error() << endl;
          exitWithStatus(EXIT_FAILURE);
        }

        if (waitpid.isNone()) {
          cerr << "Calling os::waitpid() with blocking semantics"
               << "returned asynchronously" << endl;
          exitWithStatus(EXIT_FAILURE);
        }

        if (pid == waitpid.get()) {
          break;
        }
      }

      signalSafeWriteStatus(status);
      os::close(containerStatusFd.get());
      ::_exit(EXIT_SUCCESS);
    }
  }
#endif // __WINDOWS__

  if (command->shell()) {
    // Execute the command using shell.
    os::execlp(os::Shell::name,
               os::Shell::arg0,
               os::Shell::arg1,
               command->value().c_str(),
               (char*) nullptr);
  } else {
    // Use execvp to launch the command.
    os::execvp(command->value().c_str(),
               os::raw::Argv(command->arguments()));
  }

  // If we get here, the execle call failed.
  cerr << "Failed to execute command: " << os::strerror(errno) << endl;
  UNREACHABLE();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
