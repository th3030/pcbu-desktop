#include "RFCOMMStream.Mac.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <spdlog/spdlog.h>

#import <IOBluetooth/IOBluetooth.h>

#include "platform/BluetoothHelper.h"
#include "platform/BluetoothRunLoop.Mac.h"

constexpr auto WAIT_SLICE = std::chrono::milliseconds(100);
constexpr auto WRITE_TIMEOUT = std::chrono::seconds(10);
constexpr BluetoothRFCOMMMTU FALLBACK_MTU = 127;

@interface PCBURFCOMMDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate> {
@public
  // Guarded by mutex, written on the run loop thread and read by the stream
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<uint8_t> readBuffer;
  bool isSDPDone;
  IOReturn sdpStatus;
  bool isOpenDone;
  IOReturn openStatus;
  bool isWriteDone;
  IOReturn writeStatus;
  bool isClosed;

  // Only accessed on the run loop thread
  IOBluetoothDevice *device;
  IOBluetoothRFCOMMChannel *channel;
  NSData *pendingWrite;
}
@end

// IOBluetooth does not retain the target of an SDP query, so keep it alive until the query completes.
// Completions may arrive on the main run loop instead of the BluetoothRunLoop thread, so access is synchronized.
static NSMutableSet *g_PendingQueries = [NSMutableSet set];

@implementation PCBURFCOMMDelegate
- (void)sdpQueryComplete:(IOBluetoothDevice *)sender status:(IOReturn)status {
  {
    std::lock_guard lock(mutex);
    isSDPDone = true;
    sdpStatus = status;
    cv.notify_all();
  }
  // May release the last reference, so self must not be touched afterwards
  @synchronized(g_PendingQueries) {
    [g_PendingQueries removeObject:self];
  }
}

- (void)rfcommChannelOpenComplete:(IOBluetoothRFCOMMChannel *)rfcommChannel status:(IOReturn)error {
  std::lock_guard lock(mutex);
  isOpenDone = true;
  openStatus = error;
  cv.notify_all();
}

- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel *)rfcommChannel data:(void *)dataPointer length:(size_t)dataLength {
  auto bytes = static_cast<const uint8_t *>(dataPointer);
  std::lock_guard lock(mutex);
  readBuffer.insert(readBuffer.end(), bytes, bytes + dataLength);
  cv.notify_all();
}

- (void)rfcommChannelWriteComplete:(IOBluetoothRFCOMMChannel *)rfcommChannel refcon:(void *)refcon status:(IOReturn)error {
  pendingWrite = nil;
  std::lock_guard lock(mutex);
  isWriteDone = true;
  writeStatus = error;
  cv.notify_all();
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)rfcommChannel {
  std::lock_guard lock(mutex);
  isClosed = true;
  cv.notify_all();
}
@end

static PCBURFCOMMDelegate *AsDelegate(void *delegate) {
  return (__bridge PCBURFCOMMDelegate *)delegate;
}

RFCOMMStream::RFCOMMStream(const std::string &deviceAddress, const std::atomic<bool> *isRunning, uint32_t idleTimeoutSecs)
    : m_DeviceAddress(deviceAddress), m_IsRunning(isRunning), m_IdleTimeout(idleTimeoutSecs), m_LastActivity(std::chrono::steady_clock::now()) {
  m_Delegate = (__bridge_retained void *)[[PCBURFCOMMDelegate alloc] init];
}

RFCOMMStream::~RFCOMMStream() {
  Close();
  auto delegatePtr = m_Delegate;
  m_Delegate = nullptr;
  // Release on the run loop thread, so the IOBluetooth objects are freed there as well
  BluetoothRunLoop::Run([delegatePtr]() {
    PCBURFCOMMDelegate *delegate = (__bridge_transfer PCBURFCOMMDelegate *)delegatePtr;
    delegate = nil;
  });
}

int RFCOMMStream::Connect(const uint8_t (&serviceUuid)[16], uint32_t timeoutSecs) {
  auto delegate = AsDelegate(m_Delegate);
  BluetoothDeviceAddress address{};
  if(!BluetoothHelper::ParseAddress(m_DeviceAddress, address.data)) {
    spdlog::error("Invalid Bluetooth address format: {}", m_DeviceAddress);
    return kIOReturnBadArgument;
  }

  // Waits until isDone() holds, the step timed out or the connection got stopped
  auto waitFor = [&](const std::function<bool()> &isDone) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSecs);
    std::unique_lock lock(delegate->mutex);
    while(!isDone()) {
      if(std::chrono::steady_clock::now() >= deadline || IsStopped())
        return false;
      delegate->cv.wait_for(lock, WAIT_SLICE);
    }
    return true;
  };

  // SDP lookup
  IOReturn status = kIOReturnSuccess;
  BluetoothRunLoop::Run([&]() {
    delegate->device = [IOBluetoothDevice deviceWithAddress:&address];
    if(!delegate->device) {
      status = kIOReturnNotFound;
      return;
    }
    @synchronized(g_PendingQueries) {
      [g_PendingQueries addObject:delegate];
    }
    status = [delegate->device performSDPQuery:delegate];
    if(status != kIOReturnSuccess) {
      @synchronized(g_PendingQueries) {
        [g_PendingQueries removeObject:delegate];
      }
    }
  });
  if(status != kIOReturnSuccess) {
    spdlog::error("Starting SDP query failed. (Code={:#x})", static_cast<uint32_t>(status));
    return status;
  }
  if(!waitFor([delegate]() { return delegate->isSDPDone; })) {
    if(!IsStopped())
      spdlog::error("SDP query timed out.");
    return kIOReturnTimeout;
  }
  {
    std::lock_guard lock(delegate->mutex);
    status = delegate->sdpStatus;
  }
  if(status != kIOReturnSuccess) {
    spdlog::error("SDP query failed. (Code={:#x})", static_cast<uint32_t>(status));
    return status;
  }

  // Open channel
  BluetoothRFCOMMChannelID channelId{};
  BluetoothRunLoop::Run([&]() {
    auto uuid = [IOBluetoothSDPUUID uuidWithBytes:serviceUuid length:sizeof(serviceUuid)];
    auto record = [delegate->device getServiceRecordForUUID:uuid];
    if(!record) {
      spdlog::error("SDP service not found on device. Is the app running on the phone?");
      status = kIOReturnNotFound;
      return;
    }
    if((status = [record getRFCOMMChannelID:&channelId]) != kIOReturnSuccess) {
      spdlog::error("SDP getRFCOMMChannelID failed. (Code={:#x})", static_cast<uint32_t>(status));
      return;
    }
    IOBluetoothRFCOMMChannel *channel = nil;
    status = [delegate->device openRFCOMMChannelAsync:&channel withChannelID:channelId delegate:delegate];
    delegate->channel = channel;
  });
  if(status != kIOReturnSuccess)
    return status;
  spdlog::debug("Opening RFCOMM channel {}...", channelId);
  if(!waitFor([delegate]() { return delegate->isOpenDone || delegate->isClosed; })) {
    if(!IsStopped())
      spdlog::error("Opening RFCOMM channel timed out.");
    Close();
    return kIOReturnTimeout;
  }
  {
    std::lock_guard lock(delegate->mutex);
    status = delegate->isOpenDone ? delegate->openStatus : kIOReturnNotOpen;
  }
  if(status != kIOReturnSuccess) {
    spdlog::error("Opening RFCOMM channel failed. (Code={:#x})", static_cast<uint32_t>(status));
    Close();
    return status;
  }
  m_LastActivity = std::chrono::steady_clock::now();
  return kIOReturnSuccess;
}

StreamResult RFCOMMStream::Read(uint8_t *buffer, size_t length) {
  auto delegate = AsDelegate(m_Delegate);
  std::unique_lock lock(delegate->mutex);
  if(delegate->readBuffer.empty()) {
    if(IsStopped())
      return {0, PacketError::CLOSED_CONNECTION};
    if(!delegate->isClosed)
      delegate->cv.wait_for(lock, WAIT_SLICE);
  }
  if(!delegate->readBuffer.empty()) {
    auto count = std::min(length, delegate->readBuffer.size());
    std::copy_n(delegate->readBuffer.begin(), count, buffer);
    delegate->readBuffer.erase(delegate->readBuffer.begin(), delegate->readBuffer.begin() + static_cast<std::ptrdiff_t>(count));
    m_LastActivity = std::chrono::steady_clock::now();
    return {static_cast<int>(count), PacketError::NONE};
  }
  if(delegate->isClosed)
    return {0, PacketError::CLOSED_CONNECTION};
  if(IsIdleTimedOut())
    return {0, PacketError::TIMEOUT};
  return {0, PacketError::NONE};
}

StreamResult RFCOMMStream::WriteRaw(const uint8_t *buffer, size_t length) {
  std::lock_guard writeLock(m_WriteMutex);
  auto delegate = AsDelegate(m_Delegate);
  {
    std::lock_guard lock(delegate->mutex);
    if(delegate->isClosed)
      return {0, PacketError::CLOSED_CONNECTION};
    delegate->isWriteDone = false;
  }

  IOReturn status = kIOReturnNotOpen;
  size_t chunkSize{};
  BluetoothRunLoop::Run([&]() {
    if(!delegate->channel || ![delegate->channel isOpen])
      return;
    auto mtu = [delegate->channel getMTU];
    chunkSize = std::min<size_t>(length, mtu > 0 ? mtu : FALLBACK_MTU);
    // writeAsync needs the data to stay valid until the write completes
    delegate->pendingWrite = [NSData dataWithBytes:buffer length:chunkSize];
    status = [delegate->channel writeAsync:const_cast<void *>(delegate->pendingWrite.bytes) length:static_cast<UInt16>(chunkSize) refcon:nullptr];
    if(status != kIOReturnSuccess)
      delegate->pendingWrite = nil;
  });
  if(status == kIOReturnNotOpen)
    return {0, PacketError::CLOSED_CONNECTION};
  if(status != kIOReturnSuccess) {
    spdlog::error("RFCOMM write failed. (Code={:#x})", static_cast<uint32_t>(status));
    return {0, PacketError::UNKNOWN};
  }

  std::unique_lock lock(delegate->mutex);
  if(!delegate->cv.wait_for(lock, WRITE_TIMEOUT, [delegate]() { return delegate->isWriteDone || delegate->isClosed; })) {
    spdlog::error("RFCOMM write timed out.");
    return {0, PacketError::TIMEOUT};
  }
  if(!delegate->isWriteDone)
    return {0, PacketError::CLOSED_CONNECTION};
  if(delegate->writeStatus != kIOReturnSuccess) {
    spdlog::error("RFCOMM write failed. (Code={:#x})", static_cast<uint32_t>(delegate->writeStatus));
    return {0, PacketError::CLOSED_CONNECTION};
  }
  m_LastActivity = std::chrono::steady_clock::now();
  return {static_cast<int>(chunkSize), PacketError::NONE};
}

void RFCOMMStream::Close() {
  auto delegate = AsDelegate(m_Delegate);
  if(!delegate)
    return;
  BluetoothRunLoop::Run([delegate]() {
    if(!delegate->channel)
      return;
    // No callbacks after this point, the closed state is set below
    [delegate->channel setDelegate:nil];
    [delegate->channel closeChannel];
    delegate->channel = nil;
  });
  std::lock_guard lock(delegate->mutex);
  delegate->isClosed = true;
  delegate->cv.notify_all();
}

bool RFCOMMStream::IsIdleTimedOut() const {
  if(m_IdleTimeout.count() == 0)
    return false;
  if(std::chrono::steady_clock::now() - m_LastActivity < m_IdleTimeout)
    return false;
  spdlog::error("RFCOMM idle timeout reached. (Timeout={}s)", m_IdleTimeout.count());
  return true;
}

bool RFCOMMStream::IsStopped() const {
  return m_IsRunning != nullptr && !m_IsRunning->load();
}
