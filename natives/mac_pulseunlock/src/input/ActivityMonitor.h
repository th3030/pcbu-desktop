#ifndef PCBU_MAC_ACTIVITYMONITOR_H
#define PCBU_MAC_ACTIVITYMONITOR_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>

class ActivityMonitor {
public:
  ActivityMonitor(const std::function<void()> &onActivity, bool canReadEventSource);
  ~ActivityMonitor();

  void Start(CFTimeInterval gracePeriod);
  void Stop();

  [[nodiscard]] CFTimeInterval GetSecondsSinceKeyPress() const;
  [[nodiscard]] bool IsCancelChordPressed() const;

private:
  // Pauses while the Mac sleeps, like the input timestamps, so sleep time cancels out
  static CFTimeInterval Now();

  [[nodiscard]] CFTimeInterval GetSecondsSinceUserInput() const;
  [[nodiscard]] std::string DescribeLastInput() const;
  static CFTimeInterval GetSecondsSinceHIDInput();
  static bool IsAnyDisplayAwake();

  // Empty on Macs without a lid
  [[nodiscard]] std::optional<bool> IsLidClosed() const;
  void UpdateLidState();
  [[nodiscard]] bool WasLidJustOpened(CFTimeInterval now) const;

  void Poll();
  void Fire(const std::string &reason);

  std::function<void()> m_OnActivity{};
  NSTimer *m_PollTimer{};
  id m_WakeObserver{};
  std::vector<id> m_LidObservers{};
  io_service_t m_RootDomain{};
  CFTimeInterval m_ArmTime{};
  CFTimeInterval m_GracePeriod{};
  // Only input after this time counts
  CFTimeInterval m_InputBaseline{};
  bool m_CanReadEventSource{};
  bool m_WasLidClosed{};
  CFTimeInterval m_LidOpenedAt{};

  static constexpr NSTimeInterval POLL_INTERVAL = 0.05;
  // Opening the lid counts as a key press, but only shortly after it happened
  static constexpr CFTimeInterval LID_OPEN_WINDOW = 10.0;
  // macOS can reset the input timer on background wakes without a key press. A real key press turns the display on within this time.
  static constexpr CFTimeInterval DISPLAY_WAKE_WINDOW = 2.0;
  // Keeps an already handled input from being detected again due to rounding
  static constexpr CFTimeInterval INPUT_TIME_TOLERANCE = 0.1;
};

#endif
