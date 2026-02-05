#include "UnlockHandler.h"

#include "KeyScanner.h"
#include "connection/UDPBroadcaster.h"
#include "connection/clients/BTUnlockClient.h"
#include "connection/clients/TCPUnlockClient.h"
#include "connection/servers/TCPUnlockServer.h"
#include "platform/NetworkHelper.h"
#include "storage/AppSettings.h"

#ifdef WINDOWS
#include <Windows.h>
#define KEY_LEFTCTRL VK_LCONTROL
#define KEY_LEFTALT VK_LMENU
#elif LINUX
#include <linux/input.h>
#elif APPLE
#include <Carbon/Carbon.h>
#define KEY_LEFTCTRL kVK_Control
#define KEY_LEFTALT kVK_Option
#endif

bool UnlockHandler::otherClientConnectedFirst = false;
bool UnlockHandler::netDownError = false;

UnlockHandler::UnlockHandler(const std::function<void(std::string)> &printMessage) {
  m_PrintMessage = printMessage;
}

UnlockResult UnlockHandler::GetResult(const std::string &authUser, const std::string &authProgram, std::atomic<bool> *isRunning) {
  auto settings = AppSettings::Get();
  auto netIf = NetworkHelper::GetSavedNetworkInterface();
  auto devices = PairedDevicesStorage::GetDevicesForUser(authUser);
  auto hasTCPServer = false;
  if(otherClientConnectedFirst)
    otherClientConnectedFirst = false;

  UDPBroadcaster *udpBroadcaster{};
  std::vector<BaseUnlockConnection *> connections{};
  for(const auto &device : devices) {
    BaseUnlockConnection *connection{};
    BaseUnlockConnection *btConnection1{};
    switch(device.pairingMethod) {
      case PairingMethod::TCP:
        connection = new TCPUnlockClient(device.ipAddress, device.tcpPort, device);
        break;
      case PairingMethod::BLUETOOTH:
        connection = new BTUnlockClient(device.bluetoothAddress, device, 0);
        btConnection1 = new BTUnlockClient(device.bluetoothAddress, device, 1);
        break;
      case PairingMethod::MANUAL_UDP:
      case PairingMethod::UDP: {
        if(udpBroadcaster == nullptr)
          udpBroadcaster = new UDPBroadcaster();
        auto port = device.pairingMethod == PairingMethod::UDP ? device.udpPort : device.udpManualPort;
        udpBroadcaster->AddDevice(device.id, port, device.pairingMethod == PairingMethod::MANUAL_UDP);
      }
      case PairingMethod::CLOUD_TCP:
        hasTCPServer = true;
        continue;
      default: {
        spdlog::error("Invalid pairing method.");
        continue;
      }
    }
    if(connection != nullptr) {
      connection->SetUnlockInfo(authUser, authProgram);
      connections.emplace_back(connection);
    }
    if(btConnection1) {
      btConnection1->SetUnlockInfo(authUser, authProgram);
      connections.emplace_back(btConnection1);
    }
  }
  if(hasTCPServer) {
    auto server = new TCPUnlockServer();
    server->SetUnlockInfo(authUser, authProgram);
    connections.emplace_back(server);
  }
  if(connections.empty()) {
    auto errorMsg = I18n::Get("error_not_paired", authUser);
    spdlog::error(errorMsg);
    m_PrintMessage(errorMsg);
    return UnlockResult(UnlockState::NOT_PAIRED_ERROR);
  }

  // Start servers
  std::vector<std::thread> threads{};
  AtomicUnlockResult currentResult{};
  std::atomic completed(0);
  std::mutex mutex{};
  std::condition_variable cv{};
  auto numServers = connections.size();
  threads.reserve(numServers);
  for(auto connection : connections) {
    threads.emplace_back([this, connection, numServers, isRunning, &currentResult, &completed, &cv, &mutex]() {
      auto serverResult = RunServer(connection, &currentResult, isRunning);
      completed.fetch_add(1);
      if(serverResult.state == UnlockState::SUCCESS)
        currentResult.store(serverResult);
      if(completed.load() == numServers) {
        if(currentResult.load().state != UnlockState::SUCCESS)
          currentResult.store(serverResult);
        std::lock_guard l(mutex);
        cv.notify_one();
      }
    });
  }

  // UDP Broadcast
  if(udpBroadcaster) {
    udpBroadcaster->Start();
  }

  // Wait
  std::unique_lock lock(mutex);
  cv.wait(lock, [&] { return completed.load() == numServers; });
  auto result = currentResult.load();

  // Cleanup
  if(udpBroadcaster) {
    udpBroadcaster->Stop();
    delete udpBroadcaster;
  }
  for(auto &thread : threads) {
    if(thread.joinable())
      thread.join();
  }
  for(const auto connection : connections)
    delete connection;
  return result;
}

UnlockResult UnlockHandler::RunServer(BaseUnlockConnection *connection, AtomicUnlockResult *currentResult, std::atomic<bool> *isRunning) {
  auto lastLogTime = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if(!connection->Start()) {
    auto errorMsg = I18n::Get("error_start_handler");
    spdlog::error(errorMsg);
    m_PrintMessage(errorMsg);
    return UnlockResult(UnlockState::START_ERROR);
  }

  auto connectMessage = I18n::Get(connection->IsServer() ? "wait_server_phone_connect" : "wait_client_phone_connect");
  if(connection->getClientNumber() == 0) {
    m_PrintMessage(connectMessage);
  }
  auto keyScanner = KeyScanner();
  keyScanner.Start();

  auto state = UnlockState::UNKNOWN;
  auto startTime = Utils::GetCurrentTimeMs();
  auto isWaitingForConnection = true;
  auto isFutureCancel = false;
  while(true) {
    if(currentResult->load().state == UnlockState::SUCCESS || (isRunning != nullptr && !isRunning->load())) {
      state = UnlockState::CANCELED;
      isFutureCancel = true;
      break;
    }

    if(connection->HasClient() && isWaitingForConnection) {
      if(connection->getClientNumber() != 0)
        otherClientConnectedFirst = true;
      m_PrintMessage(I18n::Get("wait_phone_unlock"));
      isWaitingForConnection = false;
    }
    if(connection->getClientNumber() == 0 && otherClientConnectedFirst) {
      m_PrintMessage(I18n::Get("wait_phone_unlock"));
    }

    state = connection->PollResult();
    if(state == UnlockState::CONNECT_ERROR && connection->IsRunning()) {
      if(connection->getClientNumber() == 0 && !otherClientConnectedFirst) {
        m_PrintMessage(I18n::Get("unlock_error_connect_retry"));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2250));
      if(connection->getClientNumber() == 0 && !otherClientConnectedFirst)
        m_PrintMessage(connectMessage);

      state = UnlockState::UNKNOWN;
      startTime = Utils::GetCurrentTimeMs();
      isWaitingForConnection = true;
      isFutureCancel = false;
    }
    if(state != UnlockState::UNKNOWN)
      break;
    if(!connection->HasClient() && Utils::GetCurrentTimeMs() - startTime > CRYPT_PACKET_TIMEOUT) {
      state = UnlockState::TIMEOUT;
      break;
    }
    if(keyScanner.GetKeyState(KEY_LEFTCTRL) && keyScanner.GetKeyState(KEY_LEFTALT)) {
      state = UnlockState::CANCELED;
      break;
    }

    if(!connection->HasClient() && !isWaitingForConnection) {
      if(connection->getClientNumber() == 0 && !otherClientConnectedFirst)
        m_PrintMessage(connectMessage);
      isWaitingForConnection = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if(connection->getClientNumber() == 0 && otherClientConnectedFirst)
    otherClientConnectedFirst = false;

  if(state == UnlockState::CONNECT_ERROR) {
    now = std::chrono::steady_clock::now();
    if(now - lastLogTime < std::chrono::seconds(1)) {
      netDownError = true;
      m_PrintMessage(I18n::Get("unlock_error_netdown"));
      isFutureCancel = true;
    }
  }

  connection->Stop();
  keyScanner.Stop();
  if(netDownError) {
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    netDownError = false;
  }

  if(!isFutureCancel)
    m_PrintMessage(UnlockStateUtils::ToString(state));
  spdlog::info("Connection result: {}", UnlockStateUtils::ToString(state));

  auto pwDec = CryptUtils::DecryptAES(connection->GetDevice().passwordEnc, connection->GetResponseData().passwordKey);
  if(!pwDec.has_value() && state == UnlockState::SUCCESS) {
    auto errorMsg = I18n::Get("error_password_decrypt");
    spdlog::error(errorMsg);
    m_PrintMessage(errorMsg);
    return UnlockResult(UnlockState::DATA_ERROR);
  }

  auto result = UnlockResult();
  result.state = state;
  result.device = connection->GetDevice();
  result.password = pwDec.has_value() ? pwDec.value() : "";
  return result;
}
