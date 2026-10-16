#ifndef PCBU_DESKTOP_BLUETOOTHUSERSSTORAGE_MAC_H
#define PCBU_DESKTOP_BLUETOOTHUSERSSTORAGE_MAC_H

#include <string>
#include <string_view>
#include <vector>

#include "PairedDevicesStorage.h"

// The users that have a Bluetooth paired phone. paired_devices.json is only readable by root, but the lock screen agent
// runs as the user and needs this to start pcbu_auth without waiting for a network, which Bluetooth does not need.
class BluetoothUsersStorage {
public:
  static bool HasBluetoothDevice(const std::string &userName);
  static void Update(const std::vector<PairedDevice> &devices);

private:
  BluetoothUsersStorage() = default;

  static constexpr std::string_view FILE_NAME = "bluetooth_users.json";
};

#endif // PCBU_DESKTOP_BLUETOOTHUSERSSTORAGE_MAC_H
