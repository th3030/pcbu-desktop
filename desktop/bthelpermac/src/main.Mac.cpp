#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "connection/BluetoothRelay.Mac.h"
#include "connection/SocketDefs.h"
#include "connection/stream/RFCOMMStream.Mac.h"
#include "platform/BluetoothHelper.h"
#include "storage/LoggingSystem.h"
#include "utils/StringUtils.h"

// macOS only: launch agent that opens RFCOMM connections for pcbu_auth, see BluetoothRelay.Mac.h.
// Not needed on Linux (BlueZ sockets work as root) or Windows (Winsock Bluetooth).

constexpr size_t RELAY_BUFFER_SIZE = 4096;
constexpr int RELAY_POLL_MS = 100;

static void RelayClient(SOCKET clientSocket) {
  auto request = BluetoothRelay::ReadLine(clientSocket, BluetoothRelay::REQUEST_TIMEOUT_MS);
  auto parts = request.has_value() ? StringUtils::Split(request.value(), " ") : std::vector<std::string>{};
  uint32_t timeoutSecs{};
  try {
    if(parts.size() != 3 || parts[0] != "CONNECT")
      throw std::runtime_error("");
    timeoutSecs = static_cast<uint32_t>(std::stoul(parts[2]));
  } catch(...) {
    spdlog::error("Invalid relay request.");
    SOCKET_CLOSE(clientSocket);
    return;
  }

  spdlog::info("Connecting to phone... (Address={})", parts[1]);
  std::atomic<bool> isRunning = true;
  RFCOMMStream stream(parts[1], &isRunning);
  auto status = stream.Connect(BluetoothRelay::SERVICE_UUID, timeoutSecs);
  if(status != 0) {
    spdlog::error("Connecting to phone failed. (Code={:#x})", static_cast<uint32_t>(status));
    auto response = fmt::format("ERR {}\n", status);
    BluetoothRelay::WriteAll(clientSocket, response.data(), response.size());
    SOCKET_CLOSE(clientSocket);
    return;
  }
  if(!BluetoothRelay::WriteAll(clientSocket, "OK\n", 3)) {
    spdlog::error("Relay client went away.");
    stream.Close();
    SOCKET_CLOSE(clientSocket);
    return;
  }
  spdlog::info("Relaying connection.");

  // Phone -> pcbu_auth
  std::thread phoneThread([&]() {
    uint8_t buffer[RELAY_BUFFER_SIZE];
    while(isRunning) {
      auto result = stream.Read(buffer, sizeof(buffer));
      if(result.bytes > 0) {
        if(!BluetoothRelay::WriteAll(clientSocket, buffer, static_cast<size_t>(result.bytes)))
          break;
      } else if(result.error != PacketError::NONE) {
        break;
      }
    }
    isRunning = false;
  });

  // pcbu_auth -> phone
  uint8_t buffer[RELAY_BUFFER_SIZE];
  while(isRunning) {
    pollfd pfd{};
    pfd.fd = clientSocket;
    pfd.events = POLLIN;
    auto result = poll(&pfd, 1, RELAY_POLL_MS);
    if(result < 0 && errno != EINTR)
      break;
    if(result <= 0)
      continue;
    auto count = read(clientSocket, buffer, sizeof(buffer));
    if(count <= 0)
      break;
    size_t written = 0;
    while(written < static_cast<size_t>(count)) {
      auto writeResult = stream.WriteRaw(buffer + written, static_cast<size_t>(count) - written);
      if(writeResult.bytes <= 0)
        break;
      written += static_cast<size_t>(writeResult.bytes);
    }
    if(written < static_cast<size_t>(count))
      break;
  }

  isRunning = false;
  phoneThread.join();
  stream.Close();
  SOCKET_CLOSE(clientSocket);
  spdlog::info("Relay closed.");
}

static int RunServer() {
  // Touching the controller also brings up the Bluetooth permission prompt on first run
  spdlog::info("Bluetooth available: {}", BluetoothHelper::IsAvailable());

  auto socketPath = BluetoothRelay::GetSocketPath();
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if(socketPath.empty() || socketPath.string().size() >= sizeof(address.sun_path)) {
    spdlog::error("Invalid socket path. (Path={})", socketPath.string());
    return 1;
  }
  std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
  std::error_code ec{};
  std::filesystem::create_directories(socketPath.parent_path(), ec);
  unlink(socketPath.c_str());

  SOCKET serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
  if(serverSocket == SOCKET_INVALID) {
    spdlog::error("socket(AF_UNIX) failed. (Code={})", errno);
    return 1;
  }
  if(bind(serverSocket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || chmod(socketPath.c_str(), 0600) < 0 ||
     listen(serverSocket, 4) < 0) {
    spdlog::error("Failed listening on socket. (Path={}, Code={})", socketPath.string(), errno);
    SOCKET_CLOSE(serverSocket);
    return 1;
  }
  spdlog::info("Bluetooth helper started. (Path={})", socketPath.string());

  while(true) {
    SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
    if(clientSocket == SOCKET_INVALID) {
      if(errno == EINTR || errno == ECONNABORTED)
        continue;
      spdlog::error("accept() failed. (Code={})", errno);
      break;
    }
    // Only pcbu_auth, which runs as root, may use the relay
    uid_t peerUid{};
    gid_t peerGid{};
    if(getpeereid(clientSocket, &peerUid, &peerGid) != 0 || peerUid != 0) {
      spdlog::warn("Rejected relay client. (Uid={})", peerUid);
      SOCKET_CLOSE(clientSocket);
      continue;
    }
    int noSigPipe = 1;
    setsockopt(clientSocket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
    std::thread(RelayClient, clientSocket).detach();
  }
  SOCKET_CLOSE(serverSocket);
  return 1;
}

int main() {
  signal(SIGPIPE, SIG_IGN);
  LoggingSystem::Init("bthelper", false, true);

  // IOBluetooth delivers some callbacks on the main run loop, so the main thread has to keep running it
  std::thread([]() {
    auto result = RunServer();
    LoggingSystem::Destroy();
    std::exit(result);
  }).detach();

  // A run loop without any source returns immediately, so keep a timer that never fires
  auto timer = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 1e10, 1e10, 0, 0, [](CFRunLoopTimerRef, void *) {}, nullptr);
  CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode);
  CFRelease(timer);
  while(true)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e10, false);
}
