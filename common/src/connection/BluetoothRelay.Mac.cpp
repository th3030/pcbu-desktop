#include "BluetoothRelay.Mac.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <pwd.h>
#include <spdlog/spdlog.h>
#include <sys/un.h>

#include "connection/SocketDefs.h"
#include "platform/PlatformHelper.h"

std::filesystem::path BluetoothRelay::GetSocketPath(const std::string &userName) {
  auto homeDir = PlatformHelper::GetUserHomeDir(userName);
  if(homeDir.empty())
    return {};
  return homeDir / "Library/Application Support/PulseUnlock/bthelper.sock";
}

SOCKET BluetoothRelay::Connect(const std::string &userName, const std::string &deviceAddress, uint32_t timeoutSecs, const std::atomic<bool> *isRunning,
                               int &status) {
  status = -1;
  auto socketPath = GetSocketPath(userName);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if(socketPath.empty() || socketPath.string().size() >= sizeof(address.sun_path)) {
    spdlog::error("Invalid Bluetooth helper socket path. (User={}, Path={})", userName, socketPath.string());
    return SOCKET_INVALID;
  }
  std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);

  SOCKET relaySocket = socket(AF_UNIX, SOCK_STREAM, 0);
  if(relaySocket == SOCKET_INVALID) {
    spdlog::error("socket(AF_UNIX) failed. (Code={})", errno);
    return SOCKET_INVALID;
  }
  int noSigPipe = 1;
  setsockopt(relaySocket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
  if(connect(relaySocket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    spdlog::error("Bluetooth helper is not reachable. Is the user logged in and the PulseUnlock background item allowed? (Path={}, Code={})",
                  socketPath.string(), errno);
    SOCKET_CLOSE(relaySocket);
    return SOCKET_INVALID;
  }

  // The socket lives in the user's home directory, so only trust a helper running as that user
  uid_t peerUid{};
  gid_t peerGid{};
  auto userEntry = getpwnam(userName.c_str());
  if(getpeereid(relaySocket, &peerUid, &peerGid) != 0 || !userEntry || peerUid != userEntry->pw_uid) {
    spdlog::error("Bluetooth helper runs as an unexpected user. (User={}, Uid={})", userName, peerUid);
    SOCKET_CLOSE(relaySocket);
    return SOCKET_INVALID;
  }

  auto request = fmt::format("CONNECT {} {}\n", deviceAddress, timeoutSecs);
  if(!WriteAll(relaySocket, request.data(), request.size())) {
    spdlog::error("Failed sending request to Bluetooth helper. (Code={})", errno);
    SOCKET_CLOSE(relaySocket);
    return SOCKET_INVALID;
  }

  // The helper looks up the service and then opens the channel, each step may take up to timeoutSecs
  auto response = ReadLine(relaySocket, timeoutSecs * 2000 + REQUEST_TIMEOUT_MS, isRunning);
  if(!response.has_value()) {
    if(isRunning == nullptr || isRunning->load())
      spdlog::error("No response from Bluetooth helper.");
    SOCKET_CLOSE(relaySocket);
    return SOCKET_INVALID;
  }
  if(response.value() == "OK") {
    status = 0;
    return relaySocket;
  }
  if(response.value().starts_with("ERR ")) {
    try {
      status = std::stoi(response.value().substr(4));
    } catch(...) {
    }
  } else {
    spdlog::error("Invalid response from Bluetooth helper.");
  }
  SOCKET_CLOSE(relaySocket);
  return SOCKET_INVALID;
}

std::optional<std::string> BluetoothRelay::ReadLine(SOCKET socket, uint32_t timeoutMs, const std::atomic<bool> *isRunning) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string line{};
  while(line.size() < MAX_LINE_LENGTH) {
    if(isRunning != nullptr && !isRunning->load())
      return {};
    auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if(remainingMs <= 0)
      return {};
    pollfd pfd{};
    pfd.fd = socket;
    pfd.events = POLLIN;
    auto result = poll(&pfd, 1, static_cast<int>(std::min<long long>(remainingMs, 100)));
    if(result < 0 && errno != EINTR)
      return {};
    if(result <= 0)
      continue;
    // Byte by byte, so nothing after the line is consumed
    char c{};
    if(read(socket, &c, 1) <= 0)
      return {};
    if(c == '\n')
      return line;
    line += c;
  }
  return {};
}

bool BluetoothRelay::WriteAll(SOCKET socket, const void *data, size_t length) {
  auto bytes = static_cast<const uint8_t *>(data);
  size_t written = 0;
  while(written < length) {
    auto count = write(socket, bytes + written, length - written);
    if(count < 0 && errno == EINTR)
      continue;
    if(count <= 0)
      return false;
    written += static_cast<size_t>(count);
  }
  return true;
}
