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

private:
  void ConnectThread();

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
};

#endif
#endif // PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
