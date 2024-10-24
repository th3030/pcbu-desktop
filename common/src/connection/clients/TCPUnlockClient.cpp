#include "TCPUnlockClient.h"

#include "connection/SocketDefs.h"
#include "storage/AppSettings.h"

#ifdef WINDOWS
#include <Ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#endif

TCPUnlockClient::TCPUnlockClient(const std::string& ipAddress, int port, const PairedDevice& device)
    : BaseUnlockConnection(device) {
    m_IP = ipAddress;
    m_Port = port;
    m_ClientSocket = (SOCKET)SOCKET_INVALID;
    m_IsRunning = false;
}

bool TCPUnlockClient::Start() {
    if(m_IsRunning)
        return true;

    WSA_STARTUP
    m_IsRunning = true;
    m_AcceptThread = std::thread(&TCPUnlockClient::ConnectThread, this);
    return true;
}

void TCPUnlockClient::Stop() {
    if(!m_IsRunning)
        return;

    if(m_ClientSocket != SOCKET_INVALID && m_HasConnection)
        write(m_ClientSocket, "CLOSE", 5);

    m_IsRunning = false;
    m_HasConnection = false;
    SOCKET_CLOSE(m_ClientSocket);
    if(m_AcceptThread.joinable())
        m_AcceptThread.join();
}

void TCPUnlockClient::ConnectThread() {
    uint32_t numRetries{};
    auto settings = AppSettings::Get();
    int connectError = 0;
    int allowedToLog = 0;
    spdlog::info("Connecting via TCP...");

#ifdef WINDOWS
    WSAEVENT event;
#endif
    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((u_short)m_Port);
    if (inet_pton(AF_INET, m_IP.c_str(), &serv_addr.sin_addr) <= 0) {
        spdlog::error("Invalid IP address.");
        m_IsRunning = false;
        m_UnlockState = UnlockState::UNK_ERROR;
        return;
    }

    socketStart:
    if (m_UnlockState == UnlockState::CONNECT_ERROR) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        m_UnlockState = UnlockState::UNKNOWN;
        connectError = 0;
    }    
    
    while (numRetries <= settings.clientConnectRetries && m_IsRunning) {
        if ((m_ClientSocket = socket(AF_INET, SOCK_STREAM, 0)) == SOCKET_INVALID) {
            spdlog::error("socket() failed. (Code={})", SOCKET_LAST_ERROR);
            m_IsRunning = false;
            m_UnlockState = UnlockState::UNK_ERROR;
            return;
        }
    #ifdef WINDOWS
        auto lastLogTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        DWORD waitResult;
        WSANETWORKEVENTS netEvents;
        int timeout = (int)settings.clientConnectTimeout * 1000;
        if (timeout <= 1000) {
            timeout = 500;
            allowedToLog = 2;
        }

        WSADATA wsa{};
        if(numRetries > 0) {
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
        if (WSAConnect(m_ClientSocket, reinterpret_cast<struct sockaddr *>(&serv_addr), sizeof(serv_addr), nullptr, nullptr, nullptr, nullptr) == SOCKET_ERROR) {
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
                spdlog::info("Connection established!");
                break;
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
        if(connect(m_ClientSocket, reinterpret_cast<struct sockaddr *>(&serv_addr), sizeof(serv_addr)) < 0) {
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
                connectError++;
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
    #endif
    }
    m_HasConnection = true;
    spdlog::info("Connection established!");
    PerformAuthFlow(m_ClientSocket);

    threadEnd:
    m_IsRunning = false;
    m_HasConnection = false;
    shutdown(m_ClientSocket, SHUT_RDWR);
    SOCKET_CLOSE(m_ClientSocket);
#ifdef WINDOWS
    WSACleanup();
    WSACloseEvent(event);
#endif
}
