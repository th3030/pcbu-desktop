#ifndef PCBU_DESKTOP_BTHELPERSERVICE_MAC_H
#define PCBU_DESKTOP_BTHELPERSERVICE_MAC_H

#include <string>

// Registers pcbu_bthelper (desktop/bthelpermac) as a launch agent of the app bundle via SMAppService.
// Must be called from the desktop app itself, as SMAppService registers agents of the calling app's bundle.
class BTHelperService {
public:
  // Registers the agent, or re-registers it so launchd runs the helper from the current app bundle.
  static bool Register(std::string &error);
  static void Unregister();

private:
  BTHelperService() = default;
};

#endif // PCBU_DESKTOP_BTHELPERSERVICE_MAC_H
