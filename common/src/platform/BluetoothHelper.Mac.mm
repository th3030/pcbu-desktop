#include "BluetoothHelper.h"

#include <cctype>
#include <condition_variable>
#include <mutex>
#include <set>
#include <spdlog/spdlog.h>
#include <thread>

#import <CoreBluetooth/CoreBluetooth.h>
#import <IOBluetooth/IOBluetooth.h>

#include "BluetoothRunLoop.Mac.h"

constexpr auto PAIRING_TIMEOUT = std::chrono::seconds(60);

@interface PCBUInquiryDelegate : NSObject <IOBluetoothDeviceInquiryDelegate>
@property(nonatomic, strong) NSMutableDictionary<NSString *, IOBluetoothDevice *> *foundDevices;
@property(nonatomic) bool isRunning;
@end

@implementation PCBUInquiryDelegate
- (instancetype)init {
  self = [super init];
  if(self) {
    _foundDevices = [NSMutableDictionary dictionary];
    _isRunning = true;
  }
  return self;
}

- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)sender device:(IOBluetoothDevice *)device {
  auto address = [device addressString];
  if(!address)
    return;
  // May run on the main run loop while ScanDevices() reads on the BluetoothRunLoop thread
  @synchronized(_foundDevices) {
    _foundDevices[address] = device;
  }
}

- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)sender error:(IOReturn)error aborted:(BOOL)aborted {
  // An inquiry ends after a few seconds, keep scanning until StopScan()
  if(!_isRunning || aborted)
    return;
  auto status = [sender start];
  if(status != kIOReturnSuccess)
    spdlog::error("Restarting Bluetooth inquiry failed. (Code={:#x})", static_cast<uint32_t>(status));
}
@end

@interface PCBUPairingDelegate : NSObject {
@public
  std::mutex mutex;
  std::condition_variable cv;
  bool isDone;
  IOReturn status;
}
@end

@implementation PCBUPairingDelegate
- (void)devicePairingFinished:(id)sender error:(IOReturn)error {
  std::lock_guard lock(mutex);
  isDone = true;
  status = error;
  cv.notify_all();
}
@end

// Only accessed on the Bluetooth run loop thread
static PCBUInquiryDelegate *g_InquiryDelegate = nil;
static IOBluetoothDeviceInquiry *g_DeviceInquiry = nil;

static std::string FormatAddress(IOBluetoothDevice *device) {
  auto address = [device getAddress];
  if(!address)
    return {};
  char str[18]{};
  snprintf(str, sizeof(str), "%02X:%02X:%02X:%02X:%02X:%02X", address->data[0], address->data[1], address->data[2], address->data[3], address->data[4],
           address->data[5]);
  return str;
}

bool BluetoothHelper::IsAvailable() {
  @autoreleasepool {
    auto authorization = [CBManager authorization];
    if(authorization == CBManagerAuthorizationDenied || authorization == CBManagerAuthorizationRestricted) {
      spdlog::warn("Bluetooth permission was not granted. (Authorization={})", static_cast<int>(authorization));
      return false;
    }
    if(authorization == CBManagerAuthorizationNotDetermined)
      spdlog::info("Bluetooth permission has not been asked for yet.");
    auto controller = [IOBluetoothHostController defaultController];
    if(!controller) {
      spdlog::warn("No Bluetooth controller found. (Authorization={})", static_cast<int>(authorization));
      return false;
    }
    auto powerState = [controller powerState];
    if(powerState != kBluetoothHCIPowerStateON) {
      spdlog::warn("Bluetooth is not powered on. (PowerState={}, Authorization={})", static_cast<int>(powerState), static_cast<int>(authorization));
      return false;
    }
    return true;
  }
}

void BluetoothHelper::StartScan() {
  BluetoothRunLoop::Run([]() {
    if(g_DeviceInquiry)
      return;
    g_InquiryDelegate = [[PCBUInquiryDelegate alloc] init];
    g_DeviceInquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:g_InquiryDelegate];
    [g_DeviceInquiry setUpdateNewDeviceNames:YES];
    auto status = [g_DeviceInquiry start];
    if(status != kIOReturnSuccess)
      spdlog::error("Starting Bluetooth inquiry failed. (Code={:#x})", static_cast<uint32_t>(status));
  });
}

void BluetoothHelper::StopScan() {
  BluetoothRunLoop::Run([]() {
    if(!g_DeviceInquiry)
      return;
    g_InquiryDelegate.isRunning = false;
    [g_DeviceInquiry stop];
    [g_DeviceInquiry setDelegate:nil];
    g_DeviceInquiry = nil;
    g_InquiryDelegate = nil;
  });
}

std::vector<BluetoothDevice> BluetoothHelper::ScanDevices() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  std::vector<BluetoothDevice> result{};
  BluetoothRunLoop::Run([&result]() {
    std::set<std::string> addresses{};
    auto addDevice = [&](IOBluetoothDevice *device) {
      auto address = FormatAddress(device);
      if(address.empty() || !addresses.insert(address).second)
        return;
      auto name = [device name];
      result.push_back({name ? std::string([name UTF8String]) : "Unknown device", address});
    };
    // A paired phone is not necessarily discoverable, so list paired devices too (like Windows does)
    for(IOBluetoothDevice *device in [IOBluetoothDevice pairedDevices])
      addDevice(device);
    if(!g_InquiryDelegate) {
      spdlog::error("Error: Scan not started");
      return;
    }
    NSArray<IOBluetoothDevice *> *foundDevices = nil;
    @synchronized(g_InquiryDelegate.foundDevices) {
      foundDevices = g_InquiryDelegate.foundDevices.allValues;
    }
    for(IOBluetoothDevice *device in foundDevices)
      addDevice(device);
  });
  return result;
}

bool BluetoothHelper::PairDevice(const BluetoothDevice &device) {
  BluetoothDeviceAddress address{};
  if(!ParseAddress(device.address, address.data)) {
    spdlog::error("Invalid Bluetooth address format: {}", device.address);
    return false;
  }

  auto delegate = [[PCBUPairingDelegate alloc] init];
  IOBluetoothDevicePair *pair = nil;
  auto isPaired = false;
  IOReturn status = kIOReturnSuccess;
  BluetoothRunLoop::Run([&]() {
    auto ioDevice = [IOBluetoothDevice deviceWithAddress:&address];
    if(!ioDevice) {
      status = kIOReturnNotFound;
      return;
    }
    if([ioDevice isPaired]) {
      isPaired = true;
      return;
    }
    pair = [IOBluetoothDevicePair pairWithDevice:ioDevice];
    [pair setDelegate:delegate];
    status = [pair start];
  });
  if(isPaired) {
    spdlog::info("Bluetooth device is already paired.");
    return true;
  }

  auto isDone = false;
  if(status == kIOReturnSuccess) {
    std::unique_lock lock(delegate->mutex);
    isDone = delegate->cv.wait_for(lock, PAIRING_TIMEOUT, [delegate]() { return delegate->isDone; });
    status = delegate->status;
  }
  BluetoothRunLoop::Run([&]() {
    if(!pair)
      return;
    if(!isDone)
      [pair stop];
    [pair setDelegate:nil];
    pair = nil;
  });

  if(status == kIOReturnSuccess && !isDone) {
    spdlog::error("Bluetooth pairing timed out.");
    return false;
  }
  if(status != kIOReturnSuccess) {
    spdlog::error("Error while bluetooth pairing. (Code={:#x})", static_cast<uint32_t>(status));
    return false;
  }
  return true;
}

bool BluetoothHelper::ParseAddress(const std::string &address, uint8_t (&bytes)[6]) {
  std::string hex{};
  for(auto c : address) {
    if(std::isxdigit(static_cast<unsigned char>(c)))
      hex += c;
    else if(c != ':' && c != '-')
      return false;
  }
  if(hex.size() != 12)
    return false;
  for(size_t i = 0; i < 6; i++)
    bytes[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
  return true;
}
