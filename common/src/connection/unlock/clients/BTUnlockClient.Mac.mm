#include "BTUnlockClient.h"

#include <unistd.h>

#include "connection/BluetoothRelay.Mac.h"
#include "connection/SocketDefs.h"
#include "connection/stream/RFCOMMStream.Mac.h"
#include "connection/stream/SocketStream.h"
#include "storage/AppSettings.h"

BTUnlockClient::BTUnlockClient(const std::string &deviceAddress, const PairedDevice &device, const bool &otherClient) : BaseUnlockConnection(device) {
  m_DeviceAddress = deviceAddress;
  m_IsRunning = false;
}

bool BTUnlockClient::Start() {
  if(m_IsRunning)
    return true;

  m_IsRunning = true;
  SetPhase(UnlockPhase::CLIENT_CONNECTING);
  m_AcceptThread = std::thread([this]() {
    try {
      ConnectThread();
    } catch(const std::exception &ex) {
      spdlog::error("BT client failed: {}", ex.what());
      m_UnlockState = UnlockState::UNK_ERROR;
      m_IsRunning = false;
      CloseStream();
    }
  });
  return true;
}

void BTUnlockClient::Stop() {
  m_IsRunning = false;
  if(m_AcceptThread.joinable())
    m_AcceptThread.join();
}

int BTUnlockClient::OpenStream(uint32_t connectTimeoutSecs, uint32_t socketTimeoutSecs) {
  // pcbu_auth runs as root outside the user's session, where IOBluetooth does not work, so go through the helper
  if(geteuid() == 0) {
    int status{};
    m_RelaySocket = BluetoothRelay::Connect(m_AuthUser, m_DeviceAddress, connectTimeoutSecs, &m_IsRunning, status);
    if(m_RelaySocket == SOCKET_INVALID)
      return status;
    m_Stream = std::make_unique<SocketStream>(m_RelaySocket, &m_IsRunning, socketTimeoutSecs);
    return 0;
  }

  auto stream = std::make_unique<RFCOMMStream>(m_DeviceAddress, &m_IsRunning, socketTimeoutSecs);
  auto status = stream->Connect(BluetoothRelay::SERVICE_UUID, connectTimeoutSecs);
  if(status == 0)
    m_Stream = std::move(stream);
  return status;
}

void BTUnlockClient::CloseStream() {
  if(m_Stream)
    m_Stream->Close();
  m_Stream.reset();
  m_RelaySocket = SOCKET_INVALID;
}

void BTUnlockClient::ConnectThread() {
  uint32_t numRetries{};
  auto settings = AppSettings::Get();
  spdlog::info("Connecting via BT...");

  while(true) {
    auto status = OpenStream(settings.clientConnectTimeout, settings.clientSocketTimeout);
    if(status == 0)
      break;
    if(m_IsRunning)
      spdlog::error("Connect timed out or failed. (Code={:#x}, Retry={})", static_cast<uint32_t>(status), numRetries);
    CloseStream();
    if(numRetries < settings.clientConnectRetries && m_IsRunning) {
      numRetries++;
      continue;
    }
    m_UnlockState = UnlockState::CONNECT_ERROR;
    m_IsRunning = false;
    return;
  }

  PerformAuthFlow(*m_Stream);
  if(!m_IsRunning && m_Phase == UnlockPhase::PHONE_UNLOCKING) {
    // SocketStream refuses writes once stopped, so write to the relay socket directly
    if(m_RelaySocket != SOCKET_INVALID)
      SocketWrite(m_RelaySocket, "CLOSE", 5);
    else
      m_Stream->WriteRaw(reinterpret_cast<const uint8_t *>("CLOSE"), 5);
  }

  m_IsRunning = false;
  CloseStream();
}
