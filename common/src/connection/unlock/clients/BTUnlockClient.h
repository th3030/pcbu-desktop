#ifndef PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#define PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#ifdef APPLE
#include "BTUnlockClient.Mac.h"
#else

#include "connection/unlock/BaseUnlockConnection.h"

class BTUnlockClient : public BaseUnlockConnection {
public:
  BTUnlockClient(const std::string& deviceAddress, const PairedDevice& device, const bool &otherClient);

  bool Start() override;
  void Stop() override;
  UnlockState PollResult() override;

private:
  void ConnectThread();
  static bool IsPhoneError(UnlockState state);

  int m_Channel;
  SOCKET m_ClientSocket;
  std::string m_DeviceAddress;
  // The global time variable
  std::chrono::steady_clock::time_point globalLastLogTime;
  // The mutex protecting globalLastLogTime
  std::mutex globalTimeMutexLastLogTime;
  static bool isAlreadyConnected;
  static bool otherClientConnectedFirst;
  static bool hasConnected;
  static bool delayConnect;
  static bool successfullConnect;
  static bool credentialSwitch;
  static std::string firstClientUsername;
  static std::string lastRememberedUsername;
  static std::string secondClientUsername;

  // An error reported through one of the two clients of a device, by device address, so the other client
  // reports it too instead of a timeout. Only errors raised after this client started apply.
  struct PairError {
    UnlockState state{};
    std::chrono::steady_clock::time_point time{};
  };
  static std::map<std::string, PairError> pairErrors;
  static std::mutex pairErrorsMutex;
  std::chrono::steady_clock::time_point m_StartTime{};
};

#endif
#endif // PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
