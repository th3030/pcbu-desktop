#ifndef PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#define PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#ifdef APPLE
#include "BTUnlockClient.Mac.h"
#else

#include "connection/BaseUnlockConnection.h"

class BTUnlockClient : public BaseUnlockConnection {
public:
    BTUnlockClient(const std::string &deviceAddress, const PairedDevice &device, const bool &otherClient);

    bool Start() override;
    void Stop() override;

private:
    void ConnectThread();

    int m_Channel;
    SOCKET m_ClientSocket;
    std::string m_DeviceAddress;
    #ifdef WINDOWS
    int timeToConnect;
    static bool clientHadConnected;
    static bool secondClientConnectedFirst;
    static bool isAlreadyConnected;
    static bool restartPending;
#endif
};

#endif
#endif //PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
