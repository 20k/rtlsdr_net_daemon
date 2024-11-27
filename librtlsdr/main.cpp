#define BUILD_DLL
#include <winsock2.h>
#include <ws2tcpip.h>
#include "main.h"
#include <stdio.h>
#include <rtl-sdr.h>
#include <stdexcept>
#include <assert.h>
#include <string>
#include <vector>

#if 0
// a sample exported function
void DLL_EXPORT SomeFunction(const LPCSTR sometext)
{
    MessageBoxA(0, sometext, "DLL Message", MB_OK | MB_ICONINFORMATION);
}
#endif

uint32_t DLL_EXPORT rtlsdr_get_device_count(void)
{
    return 1;
}

const char* DLL_EXPORT rtlsdr_get_device_name(uint32_t index)
{
    return "UNIMPLEMENTED";
}

int DLL_EXPORT rtlsdr_get_device_usb_strings(uint32_t index,
					     char* m,
					     char* p,
					     char* s)
{
    return 0;
}

int DLL_EXPORT rtlsdr_get_index_by_serial(const char* serial)
{
    return 0;
}


bool sendall(SOCKET s, addrinfo* ptr, const std::vector<char>& data)
{
    int64_t bytes_sent = 0;

    while(bytes_sent < data.size())
    {
        int count = sendto(s, data.data() + bytes_sent, data.size() - bytes_sent, 0, ptr->ai_addr, ptr->ai_addrlen);

        if(count == -1)
            return true;

        bytes_sent += count;
    }

    return false;
}


struct sock
{
    SOCKET s = INVALID_SOCKET;
    addrinfo* found_addr = nullptr;

    sock(const std::string& port)
    {
        WSADATA wsa_data;

        if(auto result = WSAStartup(MAKEWORD(2,2), &wsa_data); result != 0)
        {
            printf("WSAStartup failed: %d\n", result);
            throw std::runtime_error("WSA Failure");
        }

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* addr = nullptr;

        if(int result = getaddrinfo("127.0.0.1", port.c_str(), &hints, &addr); result != 0)
        {
            printf("Error at socket(): %ld\n", WSAGetLastError());
            WSACleanup();
            throw std::runtime_error("Sock Error");
        }

        // loop through all the results and make a socket
        for(found_addr = addr; found_addr != nullptr; found_addr = found_addr->ai_next)
        {
            if ((s = socket(found_addr->ai_family, found_addr->ai_socktype,
                    found_addr->ai_protocol)) == -1) {
                perror("talker: socket");
                continue;
            }

            break;
        }

        assert(s);
    }

    void write(const std::vector<char>& data)
    {
        assert(!sendall(s, found_addr, data));
    }
};

extern "C" DLL_EXPORT BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            // attach to process
            // return FALSE to fail DLL load
            break;

        case DLL_PROCESS_DETACH:
            // detach from process
            break;

        case DLL_THREAD_ATTACH:
            // attach to thread
            break;

        case DLL_THREAD_DETACH:
            // detach from thread
            break;
    }
    return TRUE; // succesful
}
