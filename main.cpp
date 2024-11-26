#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>

int main()
{
    WSADATA wsa_data;

    if(auto result = WSAStartup(MAKEWORD(2,2), &wsa_data); result != 0)
    {
        printf("WSAStartup failed: %d\n", result);
        return 1;
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* addr = nullptr;

    if(auto result = getaddrinfo(nullptr, "6960", &hints, &addr); result != 0)
    {
        printf("getaddrinfo failed: %d\n", result);
        WSACleanup();
        return 1;
    }

    SOCKET listen_sock = INVALID_SOCKET;

    listen_sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    if(listen_sock == INVALID_SOCKET)
    {
        printf("Error at socket(): %ld\n", WSAGetLastError());
        freeaddrinfo(addr);
        WSACleanup();
        return 1;
    }

    if(auto result = bind(listen_sock, addr->ai_addr, (int)addr->ai_addrlen); result == SOCKET_ERROR)
    {
        printf("bind failed with error: %d\n", WSAGetLastError());
        freeaddrinfo(addr);
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    /*if(listen(listen_sock, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Listen failed with error: %ld\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }*/

    while(1)
    {
        char data[1024] = {};
        int max_data = 1024;

        sockaddr_storage from = {};
        int sock_size = sizeof(from);

        int64_t numbytes = 0;

        if(numbytes = recvfrom(listen_sock, data, max_data, 0, (sockaddr*)&from, &sock_size); numbytes == -1)
        {
            printf("Error receiving from anyone\n");
            continue;
        }
    }

    return 0;
}
