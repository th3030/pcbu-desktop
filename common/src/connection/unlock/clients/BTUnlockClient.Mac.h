#ifndef PCBU_DESKTOP_BTUNLOCKCLIENT_MAC_H
#define PCBU_DESKTOP_BTUNLOCKCLIENT_MAC_H

#include <memory>

#include "connection/unlock/BaseUnlockConnection.h"

class BTUnlockClient : public BaseUnlockConnection {
public:
  BTUnlockClient(const std::string &deviceAddress, const PairedDevice &device, const bool &otherClient);

  bool Start() override;
  void Stop() override;

private:
  void ConnectThread();
  int OpenStream(uint32_t connectTimeoutSecs, uint32_t socketTimeoutSecs);
  void CloseStream();

  std::string m_DeviceAddress{};
  std::unique_ptr<ConnectionStream> m_Stream{};
  // Set when connected through pcbu_bthelper instead of directly
  SOCKET m_RelaySocket{-1};
};

#endif // PCBU_DESKTOP_BTUNLOCKCLIENT_MAC_H
