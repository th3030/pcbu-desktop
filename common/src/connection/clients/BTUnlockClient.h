#ifndef PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#define PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
#ifdef APPLE
#include "BTUnlockClient.Mac.h"
#else

#include "connection/BaseUnlockConnection.h"

class BTUnlockClient : public BaseUnlockConnection {
public:
    BTUnlockClient(const std::string& deviceAddress, const PairedDevice& device, const bool &secondServer);

    bool Start() override;
    void Stop() override;

private:
    void ConnectThread();

    int m_Channel;
    SOCKET m_ClientSocket;
    std::string m_DeviceAddress;
    static bool isAlreadyConnected;
#ifdef WINDOWS
    static int countedConnection;
    static bool restartPending;
#endif
};

#endif
#endif //PAM_PCBIOUNLOCK_BTUNLOCKCLIENT_H
