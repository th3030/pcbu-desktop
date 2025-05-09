#include "BTUnlockClient.h"

#include "connection/SocketDefs.h"
#include "platform/BluetoothHelper.h"
#include "storage/AppSettings.h"

#ifdef WINDOWS
#include <ws2bth.h>
#include <windows.h>
#include "../../natives/win-pcbiounlock/src/CUnlockCredential.h"
#define AF_BLUETOOTH AF_BTH
#define BTPROTO_RFCOMM BTHPROTO_RFCOMM
#elif LINUX
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#endif

bool BTUnlockClient::isAlreadyConnected = false;
static auto connectionTime = std::chrono::steady_clock::duration::zero();
#ifdef WINDOWS
std::string BTUnlockClient::firstClientUsername = "";
std::string BTUnlockClient::otherClientUsername = "";
bool BTUnlockClient::restartPending = false;
bool BTUnlockClient::userAccountSwitch = false;
#endif

BTUnlockClient::BTUnlockClient(const std::string& deviceAddress, const PairedDevice& device, const int &otherClient)
    : BaseUnlockConnection(device) {
    m_DeviceAddress = deviceAddress;
    m_Channel = -1;
    m_ClientSocket = (SOCKET)-1;
    m_IsRunning = false;
    m_OtherClient = otherClient;
#ifdef WINDOWS
    clientHadConnected = false;
#endif
}

bool BTUnlockClient::Start() {
    if(m_IsRunning)
        return true;

    WSA_STARTUP
    m_IsRunning = true;
    m_AcceptThread = std::thread(&BTUnlockClient::ConnectThread, this);
#ifdef WINDOWS
    if(m_OtherClient == 0) {
        firstClientUsername = m_AuthUser;
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
        otherClientUsername = m_AuthUser;
    }
#endif
    return true;
}

void BTUnlockClient::Stop() {
    if(!m_IsRunning)
        return;

    if(m_ClientSocket != -1 && m_HasConnection)
        write(m_ClientSocket, "CLOSE", 5);

    m_IsRunning = false;
    m_HasConnection = false;
    SOCKET_CLOSE(m_ClientSocket);
    if(m_AcceptThread.joinable())
        m_AcceptThread.join();
}

void BTUnlockClient::ConnectThread() {
    auto start = std::chrono::steady_clock::now();
    auto lastLogTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    uint32_t numRetries{};
    auto settings = AppSettings::Get();
    int connectError = 0;
    int allowedToLog = 6;
    if(hasConnected)
        hasConnected = false;
    if(hasSuccConnected)
        hasSuccConnected = false;
    if(isAlreadyConnected)
        isAlreadyConnected = false;
#ifdef WINDOWS
    if(restartPending)
        restartPending = false;
#endif
    spdlog::info("Connecting via BT...");
    socketStart:
    if(m_OtherClient != 0) {
        int counter = 0;
        auto timeoutVal = std::chrono::milliseconds(8000);
        while (!hasConnected) {
            if(isAlreadyConnected && counter < 1) {
                timeoutVal = std::chrono::milliseconds(12000);
                counter++;
            }
        #ifdef WINDOWS
            if(firstClientUsername != otherClientUsername)
                break;
            if(CUnlockCredential::isDeselectedSwitch)
                break;
        #else
            if(hasSuccConnected)
                break;
        #endif
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            now = std::chrono::steady_clock::now();
            if(now - lastLogTime > timeoutVal) {
                break;
            }
        }
    }

#ifdef WINDOWS
    if(m_OtherClient != 0) {
        if(hasSuccConnected && !CUnlockCredential::isDeselectedSwitch) {
            m_UnlockState = UnlockState::CANCELED;
            m_IsRunning = false;
            return;
        }
    }

    GUID guid = { 0x62182bf7, 0x97c8, 0x45f9, { 0xaa, 0x2c, 0x53, 0xc5, 0xf2, 0x00, 0x8b, 0xdf } };
    BTH_ADDR addr;
    BluetoothHelper::str2ba(m_DeviceAddress.c_str(), &addr);

    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.serviceClassId = guid;
    address.btAddr = addr;

    now = std::chrono::steady_clock::now();
    if(now - lastLogTime > std::chrono::seconds(8) && !restartPending && !isAlreadyConnected) {
        restartPending = true;
        lastLogTime = std::chrono::steady_clock::now();
        SC_HANDLE scManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (scManager == NULL) {
            spdlog::error("Failed to open SCManager. (Error={})", GetLastError());
            return;
        }

        SC_HANDLE bluetoothService = OpenServiceW(scManager, L"bthserv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
        if (bluetoothService == NULL) {
            spdlog::error("Failed to open Bluetooth service. (Error={})", GetLastError());
            CloseServiceHandle(scManager);
            return;
        }

        // Query dependent services
        DWORD bytesNeeded = 0;
        DWORD serviceCount = 0;
        EnumDependentServices(bluetoothService, SERVICE_ACTIVE, nullptr, 0, &bytesNeeded, &serviceCount);

        if (bytesNeeded > 0) {
            std::vector<BYTE> buffer(bytesNeeded);
            LPENUM_SERVICE_STATUS dependencies = reinterpret_cast<LPENUM_SERVICE_STATUS>(buffer.data());

            if (EnumDependentServices(bluetoothService, SERVICE_ACTIVE, dependencies, bytesNeeded, &bytesNeeded, &serviceCount)) {
                for (DWORD i = 0; i < serviceCount; i++) {
                    std::string dependentServiceName = dependencies[i].lpServiceName;
                    SC_HANDLE recHService = OpenService(scManager, dependentServiceName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
                    if (!recHService) {
                        spdlog::error("Failed to open service: ({}), Error=({})", dependentServiceName, GetLastError());
                    }

                    // Stop the service
                    SERVICE_STATUS recStatus;
                    if (ControlService(recHService, SERVICE_CONTROL_STOP, &recStatus)) {
                        spdlog::info("Stopping Bluetooth service...");
                        std::this_thread::sleep_for(std::chrono::seconds(2));  // Wait a moment for the service to stop
                    } else {
                        spdlog::error("Failed to stop Bluetooth service. (Error={})", GetLastError());
                    }

                    CloseServiceHandle(recHService);
                }
            }
        }

        // Stop the service
        SERVICE_STATUS status;
        if (ControlService(bluetoothService, SERVICE_CONTROL_STOP, &status)) {
            spdlog::info("Stopping Bluetooth service...");
            std::this_thread::sleep_for(std::chrono::seconds(2));  // Wait a moment for the service to stop
        } else {
            spdlog::error("Failed to stop Bluetooth service. (Error={})", GetLastError());
        }

        // Start the service
        if (StartServiceW(bluetoothService, 0, NULL)) {
            spdlog::info("Bluetooth service restarted successfully.");
        } else {
            spdlog::error("Failed to start Bluetooth service. (Error={})", GetLastError());
        }

        // Query service configuration to get the dependencies
        bytesNeeded = 0;
        QueryServiceConfig(bluetoothService, nullptr, 0, &bytesNeeded);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buffer(bytesNeeded);
            LPQUERY_SERVICE_CONFIG serviceConfig = reinterpret_cast<LPQUERY_SERVICE_CONFIG>(buffer.data());

            if (QueryServiceConfig(bluetoothService, serviceConfig, bytesNeeded, &bytesNeeded)) {
                if (serviceConfig->lpDependencies) {
                    // Dependencies are a double-null-terminated string array
                    LPSTR dependency = serviceConfig->lpDependencies;
                    while (*dependency) {
                        std::string dependentServiceName(dependency);
                        SC_HANDLE recHService = OpenService(scManager, dependentServiceName.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
                        if (!recHService) {
                            spdlog::error("Failed to open service: ({}), Error=({})", dependentServiceName.c_str(), GetLastError());
                        }
                             
                        // Start the current service
                        if (!StartService(recHService, 0, nullptr)) {
                            DWORD err = GetLastError();
                            if (err == ERROR_SERVICE_ALREADY_RUNNING) {
                                spdlog::info("({}) is already running.", dependentServiceName.c_str());
                            } else {
                                spdlog::error("Failed to start service, ({}), Error=({})", dependentServiceName.c_str(), GetLastError());
                            }
                        } else {
                            spdlog::info("Successfully started ({})", dependentServiceName.c_str());
                        }

                        CloseServiceHandle(recHService);
                        dependency += dependentServiceName.length() + 1; // Move to next string
                    }
                }
            } else {
                spdlog::error("Failed to query Bluetooth service config, Error=({})", GetLastError());
            }
        }

        CloseServiceHandle(bluetoothService);
        CloseServiceHandle(scManager);
        restartPending = false;
    }
#elif LINUX
    if(m_OtherClient != 0) {
        if(hasSuccConnected) {
            m_UnlockState = UnlockState::CANCELED;
            m_IsRunning = false;
            return;
        }
    }

    // 62182bf7-97c8-45f9-aa2c-53c5f2008bdf
    static uint8_t CHANNEL_UUID[16] = { 0x62, 0x18, 0x2b, 0xf7, 0x97, 0xc8,
                                        0x45, 0xf9, 0xaa, 0x2c, 0x53, 0xc5, 0xf2, 0x00, 0x8b, 0xdf };

    m_Channel = BluetoothHelper::FindSDPChannel(m_DeviceAddress, CHANNEL_UUID);
    if (m_Channel == -1) {
        m_IsRunning = false;
        m_UnlockState = UnlockState::CONNECT_ERROR;
        return;
    }

    struct sockaddr_rc address = { 0 };
    address.rc_family = AF_BLUETOOTH;
    address.rc_channel = m_Channel;
    str2ba(m_DeviceAddress.c_str(), &address.rc_bdaddr);
#endif
    if (m_UnlockState == UnlockState::CONNECT_ERROR) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_UnlockState = UnlockState::UNKNOWN;
        connectError = 0;
    }

    if((m_ClientSocket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)) == SOCKET_INVALID) {
        spdlog::error("socket(AF_BLUETOOTH) failed. (Code={})", SOCKET_LAST_ERROR);
        m_IsRunning = false;
        m_UnlockState = UnlockState::UNK_ERROR;
        return;
    }

    fd_set fdSet{};
    FD_SET(m_ClientSocket, &fdSet);
    struct timeval connectTimeout{};
    connectTimeout.tv_usec = 500000;
    if(!SetSocketRWTimeout(m_ClientSocket, settings.clientSocketTimeout)) {
        spdlog::error("Failed setting R/W timeout for socket. (Code={})", SOCKET_LAST_ERROR);
        m_UnlockState = UnlockState::UNK_ERROR;
        goto threadEnd;
    }

    if(!SetSocketBlocking(m_ClientSocket, false)) {
        spdlog::error("Failed setting socket to non-blocking mode. (Code={})", SOCKET_LAST_ERROR);
        m_UnlockState = UnlockState::UNK_ERROR;
        goto threadEnd;
    }
    if(connect(m_ClientSocket, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
        auto error = SOCKET_LAST_ERROR;
        if(error != SOCKET_ERROR_IN_PROGRESS && error != SOCKET_ERROR_WOULD_BLOCK) {
            spdlog::error("connect() failed. (Code={})", error);
            m_UnlockState = UnlockState::CONNECT_ERROR;
            goto threadEnd;
        }
    }
    if(select((int)m_ClientSocket + 1, nullptr, &fdSet, nullptr, &connectTimeout) <= 0) {
        if(numRetries < settings.clientConnectRetries && m_IsRunning && connectError < allowedToLog + 1) {
            if(numRetries >= allowedToLog) {
                spdlog::error("select() timed out or failed. (Code={}, Retry={}, ConnectError={})", SOCKET_LAST_ERROR, numRetries, connectError);
            }
        #ifdef WINDOWS
            if(CUnlockCredential::isDeselectedSwitch)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        #endif
            SOCKET_CLOSE(m_ClientSocket);
            numRetries++;
            connectError++;
            goto socketStart;
        }

        if(connectError > allowedToLog && numRetries < settings.clientConnectRetries) {
            spdlog::error("Device connection is hanging. (Code={})", SOCKET_LAST_ERROR);
            SOCKET_CLOSE(m_ClientSocket);
            m_UnlockState = UnlockState::CONNECT_ERROR;
            goto socketStart;
        }
        m_UnlockState = UnlockState::CONNECT_ERROR;
        goto threadEnd;
    }

    if(m_OtherClient != 0) {
    #ifdef WINDOWS
        if(!CUnlockCredential::isDeselectedSwitch) {
    #endif
            m_UnlockState = UnlockState::CANCELED;
            goto threadEnd;
    #ifdef WINDOWS
        } else {
            if(connectionTime < std::chrono::seconds(7) && !userAccountSwitch) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            }
            if(firstClientUsername != otherClientUsername) {
                userAccountSwitch = true;
                m_UnlockState = UnlockState::CANCELED;
                goto threadEnd;
            }
        }
    #endif
    } else {
        now = std::chrono::steady_clock::now();
        connectionTime = now - start;
    #ifdef WINDOWS
        if(CUnlockCredential::isDeselectedSwitch) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    #endif
    }

    m_HasConnection = true;
    isAlreadyConnected = true;
    spdlog::info("Connection established!");
    PerformAuthFlow(m_ClientSocket);

    threadEnd:
    m_IsRunning = false;
    m_HasConnection = false;
    isAlreadyConnected = false;
#ifdef WINDOWS
    if(!clientHadConnected && CUnlockCredential::isDeselectedSwitch && !userAccountSwitch && connectionTime < std::chrono::seconds(7)) {
        clientHadConnected = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        goto socketStart;
    }
#endif
    SOCKET_CLOSE(m_ClientSocket);
}
