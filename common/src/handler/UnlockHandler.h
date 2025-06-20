#ifndef PAM_PCBIOUNLOCK_UNLOCKHANDLER_H
#define PAM_PCBIOUNLOCK_UNLOCKHANDLER_H

#include <functional>
#include <future>
#include <string>

#include "UnlockState.h"
#include "connection/BaseUnlockConnection.h"
#include "storage/PairedDevicesStorage.h"

struct UnlockResult {
  UnlockResult() = default;
  explicit UnlockResult(UnlockState state) {
    this->state = state;
  }

  UnlockState state{};
  PairedDevice device{};
  std::string password{};
};

class AtomicUnlockResult {
public:
  UnlockResult load() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Data;
  }
  void store(const UnlockResult &data) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Data = data;
  }

private:
  UnlockResult m_Data{};
  mutable std::mutex m_Mutex{};
};

class UnlockHandler {
public:
  explicit UnlockHandler(const std::function<void(std::string)> &printMessage);
  UnlockResult GetResult(const std::string &authUser, const std::string &authProgram, std::atomic<bool> *isRunning = nullptr);

private:
  UnlockResult RunServer(BaseUnlockConnection *connection, AtomicUnlockResult *currentResult, std::atomic<bool> *isRunning);
  std::function<void (std::string)> m_PrintMessage{};
  static bool otherClientConnectedFirst;
};

#endif // PAM_PCBIOUNLOCK_UNLOCKHANDLER_H
