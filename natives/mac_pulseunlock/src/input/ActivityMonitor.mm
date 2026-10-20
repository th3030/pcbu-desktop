#include "ActivityMonitor.h"

#include <algorithm>
#include <limits>

#import <IOKit/IOKitLib.h>
#import <QuartzCore/QuartzCore.h>

#include <spdlog/spdlog.h>

ActivityMonitor::ActivityMonitor(const std::function<void()> &onActivity, bool canReadEventSource) {
  m_OnActivity = onActivity;
  m_CanReadEventSource = canReadEventSource;
  m_RootDomain = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));

  // The lid usually closes while the monitor is stopped, so track it all the time
  auto workspaceCenter = NSWorkspace.sharedWorkspace.notificationCenter;
  for(NSNotificationName name in @[
        NSWorkspaceWillSleepNotification, NSWorkspaceScreensDidSleepNotification, NSWorkspaceDidWakeNotification,
        NSWorkspaceScreensDidWakeNotification
      ]) {
    m_LidObservers.push_back([workspaceCenter addObserverForName:name
                                                          object:nil
                                                           queue:NSOperationQueue.mainQueue
                                                      usingBlock:^(NSNotification *) {
                                                        UpdateLidState();
                                                      }]);
  }
  UpdateLidState();
}

ActivityMonitor::~ActivityMonitor() {
  Stop();
  for(id observer : m_LidObservers)
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer];
  m_LidObservers.clear();
  if(m_RootDomain)
    IOObjectRelease(m_RootDomain);
}

void ActivityMonitor::Start(CFTimeInterval gracePeriod) {
  Stop();
  m_ArmTime = Now();
  m_GracePeriod = gracePeriod;
  m_InputBaseline = m_ArmTime + gracePeriod;
  m_PollTimer = [NSTimer scheduledTimerWithTimeInterval:POLL_INTERVAL
                                                repeats:YES
                                                  block:^(NSTimer *) {
                                                    Poll();
                                                  }];
  m_WakeObserver = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceScreensDidWakeNotification
                                                                               object:nil
                                                                                queue:NSOperationQueue.mainQueue
                                                                           usingBlock:^(NSNotification *) {
                                                                             auto now = Now();
                                                                             if(now - m_ArmTime < m_GracePeriod)
                                                                               return;
                                                                             UpdateLidState();
                                                                             // Notifications, power or Bluetooth devices also wake the screen
                                                                             if(WasLidJustOpened(now))
                                                                               Fire("lid opened (screen wake)");
                                                                             else
                                                                               spdlog::info("Screen woke without lid opening. Ignored.");
                                                                           }];
}

void ActivityMonitor::Stop() {
  [m_PollTimer invalidate];
  m_PollTimer = nil;
  if(m_WakeObserver) {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:m_WakeObserver];
    m_WakeObserver = nil;
  }
}

CFTimeInterval ActivityMonitor::Now() {
  return CACurrentMediaTime();
}

CFTimeInterval ActivityMonitor::GetSecondsSinceKeyPress() const {
  if(!m_CanReadEventSource)
    return GetSecondsSinceHIDInput();
  return std::min(CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown),
                  CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGEventFlagsChanged));
}

CFTimeInterval ActivityMonitor::GetSecondsSinceUserInput() const {
  if(!m_CanReadEventSource)
    return GetSecondsSinceHIDInput();
  auto result = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown);
  for(auto type : {kCGEventLeftMouseDown, kCGEventRightMouseDown, kCGEventOtherMouseDown})
    result = std::min(result, CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, type));
  return result;
}

std::string ActivityMonitor::DescribeLastInput() const {
  auto format = [](CFTimeInterval seconds) {
    return seconds > 365.0 * 24 * 60 * 60 ? std::string("never") : fmt::format("{:.1f}s ago", seconds);
  };
  if(!m_CanReadEventSource)
    return fmt::format("HID input {}", format(GetSecondsSinceHIDInput()));
  auto click = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseDown);
  for(auto type : {kCGEventRightMouseDown, kCGEventOtherMouseDown})
    click = std::min(click, CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, type));
  return fmt::format("key press {}, mouse click {}",
                     format(CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown)), format(click));
}

CFTimeInterval ActivityMonitor::GetSecondsSinceHIDInput() {
  auto service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
  if(!service)
    return std::numeric_limits<CFTimeInterval>::max();
  auto idleTime = IORegistryEntryCreateCFProperty(service, CFSTR("HIDIdleTime"), kCFAllocatorDefault, 0);
  IOObjectRelease(service);
  if(!idleTime)
    return std::numeric_limits<CFTimeInterval>::max();
  int64_t idleNs{};
  auto hasValue = CFGetTypeID(idleTime) == CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(idleTime), kCFNumberSInt64Type, &idleNs);
  CFRelease(idleTime);
  if(!hasValue)
    return std::numeric_limits<CFTimeInterval>::max();
  return static_cast<CFTimeInterval>(idleNs) / NSEC_PER_SEC;
}

bool ActivityMonitor::IsAnyDisplayAwake() {
  CGDirectDisplayID displays[16]{};
  uint32_t count{};
  if(CGGetOnlineDisplayList(16, displays, &count) != kCGErrorSuccess)
    return true;
  for(uint32_t i = 0; i < count; i++) {
    if(!CGDisplayIsAsleep(displays[i]))
      return true;
  }
  return false;
}

bool ActivityMonitor::IsCancelChordPressed() const {
  if(!m_CanReadEventSource)
    return false;
  auto flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
  return (flags & kCGEventFlagMaskControl) && (flags & kCGEventFlagMaskAlternate) && !(flags & kCGEventFlagMaskCommand);
}

std::optional<bool> ActivityMonitor::IsLidClosed() const {
  if(!m_RootDomain)
    return {};
  auto state = IORegistryEntryCreateCFProperty(m_RootDomain, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0);
  if(!state)
    return {};
  std::optional<bool> result{};
  if(CFGetTypeID(state) == CFBooleanGetTypeID())
    result = CFBooleanGetValue(static_cast<CFBooleanRef>(state));
  CFRelease(state);
  return result;
}

void ActivityMonitor::UpdateLidState() {
  auto isClosed = IsLidClosed();
  if(!isClosed.has_value())
    return;
  if(isClosed.value()) {
    m_WasLidClosed = true;
    return;
  }
  if(!m_WasLidClosed)
    return;
  m_WasLidClosed = false;
  m_LidOpenedAt = Now();
  spdlog::info("Lid opened.");
}

bool ActivityMonitor::WasLidJustOpened(CFTimeInterval now) const {
  return m_LidOpenedAt > 0 && now - m_LidOpenedAt < LID_OPEN_WINDOW;
}

void ActivityMonitor::Poll() {
  auto now = Now();
  auto armedFor = now - m_ArmTime;
  if(armedFor < m_GracePeriod)
    return;
  UpdateLidState();
  // The lid may open before the monitor is armed, as locking is often only detected after waking up
  if(WasLidJustOpened(now)) {
    Fire("lid opened");
    return;
  }
  auto inputTime = now - GetSecondsSinceUserInput();
  if(inputTime <= m_InputBaseline + INPUT_TIME_TOLERANCE)
    return;
  if(!IsAnyDisplayAwake()) {
    if(now - inputTime < DISPLAY_WAKE_WINDOW)
      return;
    spdlog::info("Input while display asleep. Ignored. ({})", DescribeLastInput());
    m_InputBaseline = inputTime;
    return;
  }
  Fire(fmt::format("input after {:.1f}s of waiting", armedFor));
}

void ActivityMonitor::Fire(const std::string &reason) {
  spdlog::info("Activity detected: {} ({})", reason, DescribeLastInput());
  // Counts once, so a cancel or failure right after does not start again
  m_LidOpenedAt = 0;
  Stop();
  m_OnActivity();
}
