#ifndef PCBU_DESKTOP_BLUETOOTHRELAY_MAC_H
#define PCBU_DESKTOP_BLUETOOTHRELAY_MAC_H

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "connection/BaseConnection.h"

// pcbu_auth runs as root outside the user's session, where IOBluetooth does not work. pcbu_bthelper is a launch agent
// inside the app bundle that runs in the user's session and opens the RFCOMM connection on its behalf.
// Protocol over a Unix socket: the client sends "CONNECT <address> <timeoutSecs>\n", the helper replies "OK\n" or
// "ERR <IOReturn>\n" and then relays raw bytes in both directions until either side closes.
class BluetoothRelay {
public:
  // 62182bf7-97c8-45f9-aa2c-53c5f2008bdf
  static constexpr uint8_t SERVICE_UUID[16] = {0x62, 0x18, 0x2b, 0xf7, 0x97, 0xc8, 0x45, 0xf9, 0xaa, 0x2c, 0x53, 0xc5, 0xf2, 0x00, 0x8b, 0xdf};
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;
  static constexpr size_t MAX_LINE_LENGTH = 256;

  // Socket of the helper running in the session of the given user (current user if empty).
  static std::filesystem::path GetSocketPath(const std::string &userName = {});

  // Connects to the phone through the helper of the given user.
  // Returns the relay socket, or SOCKET_INVALID with status set to the helper's IOReturn code (or -1).
  static SOCKET Connect(const std::string &userName, const std::string &deviceAddress, uint32_t timeoutSecs, const std::atomic<bool> *isRunning,
                        int &status);

  static std::optional<std::string> ReadLine(SOCKET socket, uint32_t timeoutMs, const std::atomic<bool> *isRunning = nullptr);
  static bool WriteAll(SOCKET socket, const void *data, size_t length);

private:
  BluetoothRelay() = default;
};

#endif // PCBU_DESKTOP_BLUETOOTHRELAY_MAC_H
