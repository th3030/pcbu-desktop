#ifndef ELEVATORSERVICE_H
#define ELEVATORSERVICE_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <boost/asio.hpp>
#include <boost/process.hpp>

#include "ElevatorCommands.h"
#include "IPCHelper.h"

class ElevatorService {
public:
  ElevatorService();
  ~ElevatorService();

  bool IsRunning();
  std::optional<ElevatorCommandResponse> ExecCommand(const ElevatorCommand &cmd);

private:
  bool IsRunningUnlocked();
  bool IsProcessRunning();
  bool IsLauncherRunning();
  boost::asio::io_context m_Ctx;
  std::optional<boost::process::process> m_Process;
  std::optional<uint32_t> m_ElevatorPid{};
  std::optional<std::chrono::steady_clock::time_point> m_LaunchedAt{};
  bool m_LauncherExited{};
  IPCHelper m_Ipc{};
  std::mutex m_Mutex{};
};

#endif
