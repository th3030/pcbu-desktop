#ifndef PCBU_DESKTOP_BLUETOOTHRUNLOOP_MAC_H
#define PCBU_DESKTOP_BLUETOOTHRUNLOOP_MAC_H

#include <functional>

// IOBluetooth delivers its callbacks on the run loop of the thread that started an operation.
// Neither pcbu_auth nor the std::threads of the desktop app run one, so all IOBluetooth work goes through this shared thread.
class BluetoothRunLoop {
public:
  // Runs the function on the Bluetooth run loop thread and waits until it has finished.
  static void Run(const std::function<void()> &func);

private:
  BluetoothRunLoop() = default;
};

#endif // PCBU_DESKTOP_BLUETOOTHRUNLOOP_MAC_H
