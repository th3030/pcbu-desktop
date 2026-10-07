#include "BluetoothRunLoop.Mac.h"

#include <future>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

static CFRunLoopRef g_RunLoop{};
static std::once_flag g_StartFlag{};
static thread_local bool t_IsRunLoopThread{};

static void StartRunLoopThread() {
  // Shared, since the thread may still be inside set_value() when this function returns
  auto runLoopPromise = std::make_shared<std::promise<CFRunLoopRef>>();
  auto runLoopFuture = runLoopPromise->get_future();
  std::thread([runLoopPromise]() {
    t_IsRunLoopThread = true;
    @autoreleasepool {
      auto runLoop = CFRunLoopGetCurrent();
      // A run loop without any source returns immediately, so keep a timer that never fires
      auto timer = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 1e10, 1e10, 0, 0, [](CFRunLoopTimerRef, void *) {}, nullptr);
      CFRunLoopAddTimer(runLoop, timer, kCFRunLoopDefaultMode);
      CFRelease(timer);
      CFRetain(runLoop);
      runLoopPromise->set_value(runLoop);
    }
    while(true) {
      @autoreleasepool {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
      }
    }
  }).detach();
  g_RunLoop = runLoopFuture.get();
}

void BluetoothRunLoop::Run(const std::function<void()> &func) {
  auto funcPtr = &func;
  auto invoke = [funcPtr]() {
    @autoreleasepool {
      try {
        (*funcPtr)();
      } catch(const std::exception &ex) {
        spdlog::error("Bluetooth run loop task failed: {}", ex.what());
      }
    }
  };
  if(t_IsRunLoopThread) {
    invoke();
    return;
  }

  std::call_once(g_StartFlag, StartRunLoopThread);
  auto done = dispatch_semaphore_create(0);
  CFRunLoopPerformBlock(g_RunLoop, kCFRunLoopDefaultMode, ^{
    invoke();
    dispatch_semaphore_signal(done);
  });
  CFRunLoopWakeUp(g_RunLoop);
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}
