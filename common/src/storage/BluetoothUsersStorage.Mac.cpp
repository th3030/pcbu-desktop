#include "BluetoothUsersStorage.Mac.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "AppSettings.h"
#include "shell/LocalShell.h"
#include "shell/Shell.h"
#include "utils/StringUtils.h"

bool BluetoothUsersStorage::HasBluetoothDevice(const std::string &userName) {
  auto filePath = AppSettings::GetBaseDir() / FILE_NAME;
  if(!std::filesystem::exists(filePath))
    return false;
  auto jsonData = LocalShell::ReadBytes(filePath);
  if(jsonData.empty())
    return false;

  try {
    auto users = nlohmann::json::parse(jsonData).value("users", std::vector<std::string>{});
    return std::ranges::find(users, userName) != users.end();
  } catch(const std::exception &ex) {
    spdlog::error("Failed reading Bluetooth users: {}", ex.what());
    return false;
  }
}

void BluetoothUsersStorage::Update(const std::vector<PairedDevice> &devices) {
  try {
    std::vector<std::string> users{};
    for(const auto &device : devices) {
      if(device.pairingMethod == PairingMethod::BLUETOOTH && std::ranges::find(users, device.userName) == users.end())
        users.emplace_back(device.userName);
    }
    nlohmann::json json = {
        {"users", users},
    };
    auto filePath = AppSettings::GetBaseDir() / FILE_NAME;
    auto jsonStr = json.dump();
    if(!Shell::WriteBytes(filePath, {jsonStr.begin(), jsonStr.end()})) {
      spdlog::error("Failed writing Bluetooth users to '{}'.", filePath.string());
      return;
    }
    // Readable by the lock screen agent, which runs as the user
    Shell::RunCommand(fmt::format("chmod 644 {}", StringUtils::ShellQuote(filePath.string())));
  } catch(const std::exception &ex) {
    spdlog::error("Failed writing Bluetooth users: {}", ex.what());
  }
}
