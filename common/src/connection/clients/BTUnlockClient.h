#ifndef PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#define PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#ifdef APPLE
#include "BTUnlockClient.Mac.h"
#else

#include "connection/BaseUnlockConnection.h"

class BTUnlockClient : public BaseUnlockConnection {
public:
  BTUnlockClient(const std::string& deviceAddress, const PairedDevice& device, const int &otherClient);

  bool Start() override;
  void Stop() override;

private:
  void ConnectThread();

  int m_Channel;
  SOCKET m_ClientSocket;
  std::string m_DeviceAddress;
  static bool isAlreadyConnected;
  static std::string firstClientUsername;
  static std::string otherClientUsername;
#ifdef WINDOWS
  bool clientHadConnected;
  static bool restartPending;
  static bool userAccountSwitch;
#endif
};

#endif
#endif // PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
