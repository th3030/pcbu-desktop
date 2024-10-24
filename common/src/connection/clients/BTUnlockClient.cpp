#include "BTUnlockClient.h"

#include "connection/SocketDefs.h"
#include "platform/BluetoothHelper.h"
#include "storage/AppSettings.h"

#ifdef WINDOWS
#include <ws2bth.h>
#define AF_BLUETOOTH AF_BTH
#define BTPROTO_RFCOMM BTHPROTO_RFCOMM
#define SHUT_RDWR SD_BOTH
#elif LINUX
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#endif

BTUnlockClient::BTUnlockClient(const std::string& deviceAddress, const PairedDevice& device)
    : BaseUnlockConnection(device) {
    m_DeviceAddress = deviceAddress;
    m_Channel = -1;
    m_ClientSocket = (SOCKET)-1;
    m_IsRunning = false;
}

bool BTUnlockClient::Start() {
    if(m_IsRunning)
        return true;

    WSA_STARTUP
    m_IsRunning = true;
    m_AcceptThread = std::thread(&BTUnlockClient::ConnectThread, this);
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
    uint32_t numRetries{};
    auto settings = AppSettings::Get();
    int connectError = 0;
    int allowedToLog = 0;
    bool deviceConnect = false;
    spdlog::info("Connecting via BT...");

    socketStart:
    auto lastLogTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
#ifdef WINDOWS
    WSAEVENT event;
    WSADATA wsa{};
    if(numRetries > 0 || deviceConnect) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { 
            spdlog::error("WSAStartup failed.");
            return; 
        }
    }

    GUID guid;
    if (!deviceConnect) {
        guid = { 0x00001105, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
    } else {
        guid = { 0x62182bf7, 0x97c8, 0x45f9, { 0xaa, 0x2c, 0x53, 0xc5, 0xf2, 0x00, 0x8b, 0xdf } };
    }
    BTH_ADDR addr;
    BluetoothHelper::str2ba(m_DeviceAddress.c_str(), &addr);

    if (!deviceConnect) {
        WSAQUERYSET wsaQuerySet;
        ZeroMemory(&wsaQuerySet, sizeof(wsaQuerySet));
        wsaQuerySet.dwSize = sizeof(wsaQuerySet);
        wsaQuerySet.dwNameSpace = NS_BTH;
        wsaQuerySet.lpServiceClassId = NULL;

        HANDLE hLookup;
        int result = WSALookupServiceBegin(&wsaQuerySet, LUP_CONTAINERS | LUP_RETURN_ADDR | LUP_RETURN_NAME, &hLookup);
        if (result != 0) {
            m_IsRunning = false;
            m_UnlockState = UnlockState::CONNECT_ERROR;
            return;
        }
        
        BYTE buffer[4096];
        WSAQUERYSET* pResults = reinterpret_cast<WSAQUERYSET*>(&buffer);
        DWORD bufferLength = sizeof(buffer);

        while (WSALookupServiceNext(hLookup, LUP_RETURN_ADDR | LUP_RETURN_NAME, &bufferLength, pResults) == 0) {
            // Get the Bluetooth address of the discovered device
            BTH_ADDR discoveredAddr = ((SOCKADDR_BTH*)pResults->lpcsaBuffer->RemoteAddr.lpSockaddr)->btAddr;

            // Compare with the target Bluetooth address
            if (discoveredAddr == addr) {
                // Cleanup and return true since we've found the device
                WSALookupServiceEnd(hLookup);
                break;
            }

            now = std::chrono::steady_clock::now();
            if (now - lastLogTime > std::chrono::milliseconds(500)) {
                // Cleanup after discovery process
                WSALookupServiceEnd(hLookup);
                break;
            }
        }
    }

    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.serviceClassId = guid;
    address.btAddr = addr;
#elif LINUX
    // 62182bf7-97c8-45f9-aa2c-53c5f2008bdf
    static uint8_t CHANNEL_UUID[16];
    if (!deviceConnect) {
        uint8_t tmp[] = { 0x00, 0x00, 0x11, 0x05, 0x00, 0x00,
                                        0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
        memcpy(CHANNEL_UUID, tmp, sizeof(CHANNEL_UUID));
    } else {
        uint8_t tmp[] = { 0x62, 0x18, 0x2b, 0xf7, 0x97, 0xc8,
                                        0x45, 0xf9, 0xaa, 0x2c, 0x53, 0xc5, 0xf2, 0x00, 0x8b, 0xdf };
        memcpy(CHANNEL_UUID, tmp, sizeof(CHANNEL_UUID));
    }

    m_Channel = BluetoothHelper::FindSDPChannel(m_DeviceAddress, CHANNEL_UUID);
    if (m_Channel == -1) {
        m_IsRunning = false;
        m_UnlockState = UnlockState::CONNECT_ERROR;
        return;
    }
    
    if (!deviceConnect) {
        int dev_id = hci_get_route(nullptr);
        int sock = hci_open_dev(dev_id);
        if (dev_id < 0 || sock < 0)
            return;

        int len = 8;
        int max_rsp = 255;
        auto ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));
        int num_rsp = hci_inquiry(dev_id, len, max_rsp, nullptr, &ii, IREQ_CACHE_FLUSH);
        if (num_rsp <= 0) {
            spdlog::error("hci_inquiry() failed.");
            free(ii);
            close(sock);
            return;
        }

        for (int i = 0; i < num_rsp; i++) {
            char addr[18]{};
            char name[248]{};
            ba2str(&(ii + i)->bdaddr, addr);
            memset(name, 0, sizeof(name));
            if (hci_read_remote_name(sock, &(ii + i)->bdaddr, sizeof(name), name, 0) < 0)
                strcpy(name, "Unknown device");

            if (strcmp(addr, m_DeviceAddress.c_str()) == 0) {
                break;
            }

            now = std::chrono::steady_clock::now();
            if (now - lastLogTime > std::chrono::milliseconds(500)) {
                break;
            }
        }
        free(ii);
        close(sock);
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

    while (numRetries <= settings.clientConnectRetries && m_IsRunning) {
        if((m_ClientSocket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)) == SOCKET_INVALID) {
            spdlog::error("socket(AF_BLUETOOTH) failed. (Code={})", SOCKET_LAST_ERROR);
            m_IsRunning = false;
            m_UnlockState = UnlockState::UNK_ERROR;
            return;
        }
    #ifdef WINDOWS
        DWORD waitResult;
        WSANETWORKEVENTS netEvents;
        int timeout = (int)settings.clientConnectTimeout * 1000;
        if (timeout <= 1000) {
            timeout = 500;
            allowedToLog = 2;
        }

        if(numRetries > 0 || deviceConnect) {
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { 
                spdlog::error("WSAStartup failed.");
                return; 
            }
        }
        // Create an event object for the socket
        event = WSACreateEvent();
        if (event == WSA_INVALID_EVENT) {
            spdlog::error("WSACreateEvent failed");
            m_UnlockState = UnlockState::UNK_ERROR;
            goto threadEnd;
        }

        // Set the socket to non-blocking mode and associate it with the event
        if (WSAEventSelect(m_ClientSocket, event, FD_CONNECT) == SOCKET_ERROR) {
            spdlog::error("WSAEventSelect failed. (Code={})", SOCKET_LAST_ERROR);
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            WSACloseEvent(event);
            WSACleanup();
            m_UnlockState = UnlockState::UNK_ERROR;
            goto threadEnd;
        }

        // Initiate a connection
        if (WSAConnect(m_ClientSocket, reinterpret_cast<struct sockaddr*>(&address), sizeof(address), nullptr, nullptr, nullptr, nullptr) == SOCKET_ERROR) {
            if (SOCKET_LAST_ERROR != SOCKET_ERROR_IN_PROGRESS && SOCKET_LAST_ERROR != SOCKET_ERROR_WOULD_BLOCK) { // Non-blocking socket in progress
                spdlog::error("WSAConnect failed. (Code={})", SOCKET_LAST_ERROR);
                shutdown(m_ClientSocket, SHUT_RDWR);
                SOCKET_CLOSE(m_ClientSocket);
                WSACloseEvent(event);
                WSACleanup();
                m_UnlockState = UnlockState::CONNECT_ERROR;
                goto threadEnd;
            }
        }

        // Wait for the connection event (non-blocking)
        waitResult = WSAWaitForMultipleEvents(1, &event, TRUE, timeout, FALSE);
        if (waitResult == WSA_WAIT_TIMEOUT && numRetries < settings.clientConnectRetries && m_IsRunning && connectError < allowedToLog + 1) {
            if(numRetries >= allowedToLog) {
                spdlog::error("WSAWaitForMultipleEvents() timed out or failed. (Code={}, Retry={}, ConnectError={})", SOCKET_LAST_ERROR, numRetries, connectError);
            }
            if (SOCKET_LAST_ERROR == SOCKET_ERROR_WOULD_BLOCK) {
                connectError++;
            }
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            WSACloseEvent(event);
            WSACleanup();
            numRetries++;
            continue;
        } else if (waitResult == WSA_WAIT_FAILED && SOCKET_LAST_ERROR != SOCKET_ERROR_IN_PROGRESS) {
            spdlog::error("WSAWaitForMultipleEvents() failed. (Code={})", SOCKET_LAST_ERROR);
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            WSACloseEvent(event);
            WSACleanup();
            m_UnlockState = UnlockState::UNK_ERROR;
            goto threadEnd;
        }

        // If the computer can see the phone but the phone hangs with the connection abort the connection.
        if (connectError > allowedToLog) {
            spdlog::error("Device connection is hanging. (Code={})", SOCKET_LAST_ERROR);
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            WSACloseEvent(event);
            WSACleanup();
            m_UnlockState = UnlockState::CONNECT_ERROR;
            goto socketStart;
        }

        // Check what kind of event occurred
        if (WSAEnumNetworkEvents(m_ClientSocket, event, &netEvents) == SOCKET_ERROR) {
            spdlog::error("WSAEnumNetworkEvents failed. (Code={})", SOCKET_LAST_ERROR);
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            WSACloseEvent(event);
            WSACleanup();
            m_UnlockState = UnlockState::UNK_ERROR;
            goto threadEnd;
        }

        // Check for successful connection
        if (netEvents.lNetworkEvents & FD_CONNECT) {
            if (netEvents.iErrorCode[FD_CONNECT_BIT] == 0) {
                if (!deviceConnect) {
                    deviceConnect = true;
                    spdlog::info("OPP Connect successfull!");                    
                    shutdown(m_ClientSocket, SHUT_RDWR);
                    SOCKET_CLOSE(m_ClientSocket);
                    WSACloseEvent(event);
                    WSACleanup();
                    goto socketStart;
                } else {
                    spdlog::info("Connection established!");
                    break;
                }
            } else {
                now = std::chrono::steady_clock::now();
                if (now - lastLogTime > std::chrono::seconds(settings.clientConnectTimeout) || numRetries == 0) {
                    spdlog::error("Connection failed. (Code={}, Retry={})", netEvents.iErrorCode[FD_CONNECT_BIT], numRetries);
                    lastLogTime = now;  // Update the time of last log
                }
                if (netEvents.iErrorCode[FD_CONNECT_BIT] == SOCKET_ERROR_WOULD_BLOCK || netEvents.iErrorCode[FD_CONNECT_BIT] == WSAEADDRINUSE || netEvents.iErrorCode[FD_CONNECT_BIT] == WSAEADDRNOTAVAIL || netEvents.iErrorCode[FD_CONNECT_BIT] == SOCKET_ERROR_TIMEOUT) {
                    if (numRetries < settings.clientConnectRetries && m_IsRunning) {
                        shutdown(m_ClientSocket, SHUT_RDWR);
                        SOCKET_CLOSE(m_ClientSocket);
                        WSACloseEvent(event);
                        WSACleanup();
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        if (numRetries == 0){
                            numRetries++;
                        }
                        continue;
                    }
                }
                m_UnlockState = UnlockState::CONNECT_ERROR;
                goto threadEnd;
            }
        }
    #else
        fd_set fdSet{};
        FD_SET(m_ClientSocket, &fdSet);
        struct timeval connectTimeout{};
        long timeoutsec = (long)settings.clientConnectTimeout;
        long timeoutusec = timeoutsec * 1000000;
        timeoutusec = timeoutusec - 500000;
        if (timeoutusec < 1000000) {
            connectTimeout.tv_usec = 500000;
            allowedToLog = 2;
        } else {
            connectTimeout.tv_sec = timeoutsec;
        }
        if (!SetSocketRWTimeout(m_ClientSocket, settings.clientSocketTimeout)) {
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
                if (SOCKET_LAST_ERROR == SOCKET_ERROR_WOULD_BLOCK) {
                    connectError++;
                }
                shutdown(m_ClientSocket, SHUT_RDWR);
                SOCKET_CLOSE(m_ClientSocket);
                numRetries++;
                continue;
            }
            if (connectError > allowedToLog) {
                spdlog::error("Device connection is hanging. (Code={})", SOCKET_LAST_ERROR);
                shutdown(m_ClientSocket, SHUT_RDWR);
                SOCKET_CLOSE(m_ClientSocket);
                m_UnlockState = UnlockState::CONNECT_ERROR;
                goto socketStart;
            }
            m_UnlockState = UnlockState::CONNECT_ERROR;
            goto threadEnd;
        }
        
        if(!deviceConnect) {
            deviceConnect = true;
            spdlog::info("OPP connection successfull!");
            shutdown(m_ClientSocket, SHUT_RDWR);
            SOCKET_CLOSE(m_ClientSocket);
            goto socketStart;
        } else {
            spdlog::info("Connection established!");
        }
    #endif
    }
    m_HasConnection = true;
    PerformAuthFlow(m_ClientSocket);

    threadEnd:
    m_IsRunning = false;
    m_HasConnection = false;
    shutdown(m_ClientSocket, SHUT_RDWR);
    SOCKET_CLOSE(m_ClientSocket);
#ifdef WINDOWS
    WSACloseEvent(event);
    WSACleanup();
#endif
}
