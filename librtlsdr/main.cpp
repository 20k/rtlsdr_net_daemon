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
#include <optional>
#include <atomic>
#include <SFML/System/Sleep.hpp>
#include <ranges>

#if 0
// a sample exported function
void DLL_EXPORT SomeFunction(const LPCSTR sometext)
{
    MessageBoxA(0, sometext, "DLL Message", MB_OK | MB_ICONINFORMATION);
}
#endif

bool sendall(SOCKET s, addrinfo* ptr, const std::vector<char>& data)
{
    int64_t bytes_sent = 0;

    while(bytes_sent < (int64_t)data.size())
    {
        int count = sendto(s, data.data() + bytes_sent, data.size() - bytes_sent, 0, ptr->ai_addr, ptr->ai_addrlen);

        if(count == -1)
            return true;

        bytes_sent += count;
    }

    return false;
}

std::vector<char> readall(SOCKET s, sockaddr_storage* their_addr)
{
    std::vector<char> bufsize;
    bufsize.resize(10000);

    int from_len = sizeof(*their_addr);

    int len = recvfrom(s, bufsize.data(), bufsize.size(), 0, (sockaddr*)their_addr, &from_len);

    assert(len != -1);

    assert(len <= (int)bufsize.size());

    bufsize.resize(len);

    return bufsize;
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
            printf("Error at socket(): %d\n", WSAGetLastError());
            WSACleanup();
            throw std::runtime_error("Sock Error");
        }

        int yes = 1;

        for(found_addr = addr; found_addr != nullptr; found_addr = found_addr->ai_next)
        {
            if(s = socket(found_addr->ai_family, found_addr->ai_socktype,
                    found_addr->ai_protocol); s == (uint32_t)SOCKET_ERROR)
            {
                perror("talker: socket");
                continue;
            }

            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(int));
            int size = 1024 * 1024 * 5;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(int));

            break;
        }

        assert(s);
    }

    void write(const std::vector<char>& data)
    {
        assert(!sendall(s, found_addr, data));
    }

    void write(unsigned int in)
    {
        std::vector<char> data;
        data.resize(sizeof(in));

        memcpy(data.data(), &in, sizeof(in));

        return write(data);
    }

    std::vector<char> read()
    {
        sockaddr_storage store;

        return readall(s, &store);
    }
};

template<typename T>
std::optional<T> read_pop(std::vector<char>& in)
{
    if(in.size() < sizeof(T))
        return std::nullopt;

    T ret = {};
    memcpy(&ret, in.data(), sizeof(T));
    in = std::vector<char>(in.begin() + sizeof(T), in.end());
    return ret;
}

void add(std::vector<char>& in, int v)
{
    auto len = in.size();

    in.resize(in.size() + sizeof(int));

    memcpy(in.data() + len, &v, sizeof(int));
}

void add(std::vector<char>& in, uint32_t v)
{
    auto len = in.size();

    in.resize(in.size() + sizeof(v));

    memcpy(in.data() + len, &v, sizeof(v));
}

void add(std::vector<char>& in, const std::vector<int> v)
{
    for(auto& i : v)
        add(in, i);
}


sock* data_sock2 = nullptr;
sock* query_sock2 = nullptr;

sock* get_data_sock()
{
    if(data_sock2 == nullptr)
        data_sock2 = new sock("6960");

    return data_sock2;
}

sock* get_query_sock()
{
    if(query_sock2 == nullptr)
        query_sock2 = new sock("6961");

    return query_sock2;
}

void data_write(char type, auto what)
{
    std::vector<char> data;
    data.push_back(type);

    add(data, what);

    get_data_sock()->write(data);
}

std::vector<char> query_read(char type)
{
    get_query_sock()->write(std::vector<char>{type});

    return get_query_sock()->read();
}

uint32_t DLL_EXPORT rtlsdr_get_device_count(void)
{
    return 1;
}

const char* DLL_EXPORT rtlsdr_get_device_name(uint32_t index)
{
    std::vector<char> out;
    out.push_back(0x11);
    add(out, index);

    get_query_sock()->write(out);

    auto data = get_query_sock()->read();

    std::string* leaked = new std::string(data.begin(), data.end());

    return leaked->c_str();
}

int DLL_EXPORT rtlsdr_get_device_usb_strings(uint32_t index,
					     char* m,
					     char* p,
					     char* s)
{
    std::vector<char> out;
    out.push_back(0x22);
    add(out, index);

    for(int i=0; i < 256; i++)
        m[i] = 0;
    for(int i=0; i < 256; i++)
        p[i] = 0;
    for(int i=0; i < 256; i++)
        s[i] = 0;

    get_query_sock()->write(out);

    std::vector<char> data = get_query_sock()->read();

    std::string as_str(data.begin(), data.end());

    int which_word = 0;

    for(const auto word : std::views::split(as_str, '\0'))
    {
        std::string_view sword(word);

        assert(sword.size() < 256);

        if(which_word == 0)
            memcpy(m, sword.data(), sword.size());

        if(which_word == 1)
            memcpy(p, sword.data(), sword.size());

        if(which_word == 2)
            memcpy(s, sword.data(), sword.size());

        which_word++;
    }

    return 0;
}

int DLL_EXPORT rtlsdr_get_index_by_serial(const char* serial)
{
    return 0;
}

char data_storage[1024] = {};

DLL_EXPORT int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index)
{
    dev[0] = (rtlsdr_dev_t*)data_storage;
    return 0;
}

DLL_EXPORT int rtlsdr_close(rtlsdr_dev_t *dev)
{
    return 0;
}

DLL_EXPORT int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
    return 0;
}

DLL_EXPORT int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
    get_query_sock()->write({0x19});

    std::vector<char> read = get_query_sock()->read();

    assert(read.size() == 8);

    uint32_t freq1 = {};
    uint32_t freq2 = {};

    memcpy(&freq1, read.data(), sizeof(uint32_t));
    memcpy(&freq2, read.data() + sizeof(uint32_t), sizeof(uint32_t));

    if(rtl_freq)
        *rtl_freq = freq1;

    if(tuner_freq)
        *tuner_freq = freq2;

    return 0;
}

DLL_EXPORT int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char* m, char* p, char* s)
{
    for(int i=0; i < 256; i++)
        m[i] = 0;
    for(int i=0; i < 256; i++)
        p[i] = 0;
    for(int i=0; i < 256; i++)
        s[i] = 0;

    auto data = query_read(0x23);

    std::string as_str(data.begin(), data.end());

    int which_word = 0;

    for(const auto word : std::views::split(as_str, '\0'))
    {
        std::string_view sword(word);

        assert(sword.size() < 256);

        if(which_word == 0)
            memcpy(m, sword.data(), sword.size());

        if(which_word == 1)
            memcpy(p, sword.data(), sword.size());

        if(which_word == 2)
            memcpy(s, sword.data(), sword.size());

        which_word++;
    }

    return 0;

    return 0;
}

DLL_EXPORT int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t* data, uint8_t offset, uint16_t len)
{
    return 0;
}

DLL_EXPORT int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
    return -3;
}

DLL_EXPORT int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
    data_write(0x01, freq);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
    auto result = query_read(0x20);

    return read_pop<uint32_t>(result).value();
}

DLL_EXPORT int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
    data_write(0x05, ppm);

    return 0;
}

DLL_EXPORT int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
    auto result = query_read(0x12);

    return read_pop<int>(result).value();
}

DLL_EXPORT enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
    return RTLSDR_TUNER_R828D;
}

DLL_EXPORT int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
    get_query_sock()->write({0x14});

    auto result = get_query_sock()->read();

    uint32_t len = read_pop<uint32_t>(result).value();

    if(gains)
    {
        for(int i=0; i < (int)len; i++)
        {
            gains[i] = read_pop<int>(result).value();
        }
    }

    return len;
}

DLL_EXPORT int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain)
{
    data_write(0x04, gain);
    return 0;
}

DLL_EXPORT int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw)
{
    data_write(0x21, bw);

    return 0;
}

DLL_EXPORT int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
    auto data = query_read(0x15);

    return read_pop<int>(data).value();
}

DLL_EXPORT int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain)
{
    return 0;
}

DLL_EXPORT int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual)
{
    data_write(0x03, manual);

    return 0;
}

DLL_EXPORT int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate)
{
    data_write(0x02, rate);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
    auto data = query_read(0x16);

    return read_pop<uint32_t>(data).value();
}

DLL_EXPORT int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on)
{
    return 0;
}

DLL_EXPORT int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on)
{
    data_write(0x08, on);

    return 0;
}

DLL_EXPORT int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
    data_write(0x09, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
    auto dat = query_read(0x17);
    return read_pop<int>(dat).value();
}

DLL_EXPORT int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
    data_write(0x0a, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
    auto dat = query_read(0x18);
    return read_pop<int>(dat).value();
}

DLL_EXPORT int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
    return 0;
}

DLL_EXPORT int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
    std::vector<char> data = get_data_sock()->read();

    assert(buf);

    while((int)data.size() < len)
    {
        auto next = get_data_sock()->read();

        data.insert(data.end(), next.begin(), next.end());
    }

    if((int)data.size() > len)
    {
        int extra = (int)data.size() - len;
        data = std::vector<char>(data.begin() + extra, data.end());
    }

    if(n_read)
        *n_read = data.size();

    memcpy(buf, data.data(), len);
    return 0;
}

std::atomic_int cancelled{0};

DLL_EXPORT int rtlsdr_wait_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx)
{
    return rtlsdr_read_async(dev, cb, ctx, 0, 0);
}

std::mutex mut;
bool has_ever_asked_for_data = false;

DLL_EXPORT int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len)
{
    cancelled = 0;

    std::vector<unsigned char> next_data;
    int pop_size = 65536/4;

    {
        std::lock_guard guard(mut);

        if(!has_ever_asked_for_data)
        {
            has_ever_asked_for_data = true;
            get_data_sock()->write(std::vector<char>{0x0f});
        }
    }

    while(!cancelled)
    {
        auto data = get_data_sock()->read();

        next_data.insert(next_data.end(), data.begin(), data.end());

        if((int)next_data.size() >= pop_size)
        {
            cb((unsigned char*)next_data.data(), (uint32_t)next_data.size(), ctx);

            next_data.clear();
        }
        else
        {
            //sf::sleep(sf::milliseconds(1));
        }
    }

    return 0;
}

DLL_EXPORT int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
    cancelled = 1;
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
    data_write(0x0e, on);
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on)
{
    return 0;
}

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
