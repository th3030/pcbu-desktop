#include "BTHelperService.Mac.h"

#include <spdlog/spdlog.h>

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

// Located at Contents/Library/LaunchAgents/ inside the app bundle
static NSString *const AGENT_PLIST_NAME = @"com.meisapps.PulseUnlock.bthelper.plist";

static std::string ToString(NSError *error) {
  if(!error || !error.localizedDescription)
    return "Unknown error";
  return [error.localizedDescription UTF8String];
}

bool BTHelperService::Register(std::string &error) {
  @autoreleasepool {
    auto service = [SMAppService agentServiceWithPlistName:AGENT_PLIST_NAME];
    NSError *nsError = nil;
    if(service.status == SMAppServiceStatusEnabled && ![service unregisterAndReturnError:&nsError])
      spdlog::warn("Unregistering Bluetooth helper failed: {}", ToString(nsError));

    nsError = nil;
    auto isRegistered = [service registerAndReturnError:&nsError];
    if(service.status == SMAppServiceStatusRequiresApproval) {
      error = "The background item needs to be allowed in System Settings > General > Login Items.";
      [SMAppService openSystemSettingsLoginItems];
      return false;
    }
    if(!isRegistered) {
      error = ToString(nsError);
      return false;
    }
    return true;
  }
}

void BTHelperService::Unregister() {
  @autoreleasepool {
    auto service = [SMAppService agentServiceWithPlistName:AGENT_PLIST_NAME];
    if(service.status == SMAppServiceStatusNotRegistered || service.status == SMAppServiceStatusNotFound)
      return;
    NSError *nsError = nil;
    if(![service unregisterAndReturnError:&nsError])
      spdlog::warn("Unregistering Bluetooth helper failed: {}", ToString(nsError));
  }
}
