#include <cstdint>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#ifndef WINDOWS
#include <cstdlib>
#endif
#ifdef APPLE
#include <cerrno>
#include <sys/event.h>
#include <unistd.h>
#endif

#include "platform/PlatformHelper.h"
#include "shell/ElevatorCommands.h"
#include "shell/IPCHelper.h"
#include "shell/LocalShell.h"
#include "storage/LoggingSystem.h"

class ElevatorApp {
public:
  static int Run(int argc, char *argv[]);

private:
  ElevatorApp() = default;

  static int RunMain(int argc, char *argv[]);
  static bool SendResponse(IPCHelper &ipc, const ElevatorCommandResponse &resp);
  static ElevatorCommandResponse HandleCommand(const ElevatorCommand &cmd);
  static ElevatorCommandResponse ToResponse(bool success);
};

bool ElevatorApp::SendResponse(IPCHelper &ipc, const ElevatorCommandResponse &resp) {
  auto msg = resp.ToMessage();
  if(IPCHelper::IsTooLarge(msg)) {
    spdlog::error("[Elevator] Response too large. ({}, JsonSize={})", resp.ToString(), msg.json.size());
    return ipc.WriteMessage(ElevatorCommandResponse(true, "Response too large.").ToMessage());
  }
  spdlog::debug("[Elevator] Sending response... ({})", resp.ToString());
  if(!ipc.WriteMessage(msg)) {
    spdlog::error("[Elevator] Failed to send response. ({})", resp.ToString());
    return false;
  }
  return true;
}

ElevatorCommandResponse ElevatorApp::HandleCommand(const ElevatorCommand &cmd) {
  try {
    switch(cmd.type) {
      case ElevatorCommandType::RUN_CMD:
        return ElevatorCommandResponse(LocalShell::RunCommand(cmd.args.at(0)));
      case ElevatorCommandType::READ_BYTES: {
        std::error_code ec{};
        auto fileSize = std::filesystem::file_size(cmd.args.at(0), ec);
        if(!ec && fileSize > IPCHelper::MAX_BLOB_SIZE)
          return ElevatorCommandResponse(true, "File too large.");
        return ElevatorCommandResponse(LocalShell::ReadBytes(cmd.args.at(0)));
      }
      case ElevatorCommandType::WRITE_BYTES:
        return ToResponse(LocalShell::WriteBytes(cmd.args.at(0), cmd.dataBytes));
      case ElevatorCommandType::CREATE_FILE: {
        auto isDir = cmd.args.at(1) == "true";
        return ToResponse(isDir ? LocalShell::CreateDir(cmd.args.at(0)) : LocalShell::CreateFile(cmd.args.at(0)));
      }
      case ElevatorCommandType::REMOVE:
        return ToResponse(LocalShell::Remove(cmd.args.at(0)));
      case ElevatorCommandType::PROTECT_FILE: {
        auto enabled = cmd.args.at(1) == "true";
        return ToResponse(LocalShell::ProtectFile(cmd.args.at(0), enabled));
      }
      default:
        return ElevatorCommandResponse(true, "Unknown command.");
    }
  } catch(const std::exception &ex) {
    spdlog::error("[Elevator] Failed executing command. ({}, Exception={})", cmd.ToString(), ex.what());
    return ElevatorCommandResponse(true, ex.what());
  }
}

ElevatorCommandResponse ElevatorApp::ToResponse(bool success) {
  return success ? ElevatorCommandResponse() : ElevatorCommandResponse(true, "Command failed.");
}

int ElevatorApp::Run(int argc, char *argv[]) {
#ifndef WINDOWS
  auto homeDir = PlatformHelper::GetUserHomeDir();
  setenv("HOME", homeDir.empty() ? "/" : homeDir.c_str(), 1);
  unsetenv("ZDOTDIR");
#endif
  LoggingSystem::Init("elevator", false, true);
  auto result = RunMain(argc, argv);
  LoggingSystem::Destroy();
  return result;
}

int ElevatorApp::RunMain(int argc, char *argv[]) {
  if(argc != 3) {
    spdlog::error("Invalid args.");
    return 1;
  }

  uint32_t expectedPid{};
  try {
    expectedPid = static_cast<uint32_t>(std::stoul(argv[2]));
    if(expectedPid == 0)
      throw std::runtime_error("");
  } catch(...) {
    spdlog::error("Invalid desktop pid.");
    return 1;
  }

  IPCHelper ipc{};
  if(!ipc.Connect(argv[1], 30000)) {
    spdlog::error("Failed connecting to desktop IPC.");
    return 1;
  }

  auto peerPid = ipc.GetPeerPid();
  if(!peerPid.has_value() || peerPid.value() != expectedPid) {
    spdlog::error("Elevator IPC peer mismatch. (Pid={})", peerPid.value_or(0));
    return 1;
  }

#ifdef APPLE
  // The elevator runs detached from osascript, so also stop when the desktop process exits,
  // even if its IPC socket is kept open by another process.
  int procQueue = kqueue();
  struct kevent procWatch{};
  EV_SET(&procWatch, expectedPid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if(procQueue == -1 || kevent(procQueue, &procWatch, 1, nullptr, 0, nullptr) == -1) {
    spdlog::error("Failed watching desktop process. (Code={})", errno);
    if(procQueue != -1)
      close(procQueue);
    return 1;
  }
  auto desktopExited = false;
  auto isDesktopRunning = [procQueue, &desktopExited]() {
    if(!desktopExited) {
      struct kevent event{};
      struct timespec noWait{};
      desktopExited = kevent(procQueue, nullptr, 0, &event, 1, &noWait) == 1;
      if(desktopExited)
        spdlog::info("[Elevator] Desktop process exited.");
    }
    return !desktopExited;
  };
#else
  auto isDesktopRunning = []() { return true; };
#endif
  spdlog::info("[Elevator] Connected to desktop. (Pid={})", expectedPid);

  while(true) {
    spdlog::debug("[Elevator] Reading command...");
    auto msg = ipc.ReadMessage(isDesktopRunning, IPCHelper::NO_TIMEOUT);
    if(!msg.has_value()) {
      spdlog::info("[Elevator] IPC connection closed.");
      break;
    }

    auto cmd = ElevatorCommand::FromMessage(std::move(msg.value()));
    if(!cmd.has_value()) {
      SendResponse(ipc, ElevatorCommandResponse(true, "Invalid command."));
      continue;
    }
    spdlog::debug("[Elevator] Running command... ({})", cmd.value().ToString());
    auto resp = HandleCommand(cmd.value());
    if(resp.isError)
      spdlog::error("[Elevator] Command failed. ({}, {})", cmd.value().ToString(), resp.ToString());
    SendResponse(ipc, resp);
  }
#ifdef APPLE
  close(procQueue);
#endif
  return 0;
}

int main(int argc, char *argv[]) {
  return ElevatorApp::Run(argc, argv);
}
