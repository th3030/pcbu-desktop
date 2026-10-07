#ifndef PCBU_DESKTOP_RFCOMMSTREAM_MAC_H
#define PCBU_DESKTOP_RFCOMMSTREAM_MAC_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "connection/stream/ConnectionStream.h"

// RFCOMM connection to a remote device via IOBluetooth. All IOBluetooth calls run on the BluetoothRunLoop thread.
class RFCOMMStream : public ConnectionStream {
public:
  explicit RFCOMMStream(const std::string &deviceAddress, const std::atomic<bool> *isRunning = nullptr, uint32_t idleTimeoutSecs = 0);
  ~RFCOMMStream() override;
  RFCOMMStream(const RFCOMMStream &) = delete;
  RFCOMMStream &operator=(const RFCOMMStream &) = delete;

  // Finds the RFCOMM channel of the service via SDP and opens it. Each step may take up to timeoutSecs.
  // Returns 0 on success, otherwise an IOReturn code.
  int Connect(const uint8_t (&serviceUuid)[16], uint32_t timeoutSecs);

  StreamResult Read(uint8_t *buffer, size_t length) override;
  // Unlike Read(), writes still go through after isRunning turned false, so the CLOSE signal can be sent.
  StreamResult WriteRaw(const uint8_t *buffer, size_t length) override;
  void Close() override;

private:
  [[nodiscard]] bool IsIdleTimedOut() const;
  [[nodiscard]] bool IsStopped() const;

  void *m_Delegate{};
  std::string m_DeviceAddress{};
  const std::atomic<bool> *m_IsRunning{};
  std::chrono::seconds m_IdleTimeout{};
  std::chrono::steady_clock::time_point m_LastActivity{};
  std::mutex m_WriteMutex{};
};

#endif // PCBU_DESKTOP_RFCOMMSTREAM_MAC_H
