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

FILE* get_file()
{
    static FILE* file = fopen("./dump.txt", "a");
    return file;
}

//#define LOG(x) fwrite(x, strlen(x), 1, get_file())

#define LOG(x)
#define FLOG(x)
//#define FLOG(x) fwrite(x.c_str(), x.size(), 1, get_file())

struct sock_view
{
    sockaddr* addr = nullptr;
    int len = 0;

    sock_view(sockaddr_storage& in)
    {
        addr = (sockaddr*)&in;
        len = sizeof(sockaddr_storage);
    }

    sock_view(addrinfo* inf)
    {
        addr = inf->ai_addr;
        len = inf->ai_addrlen;
    }
};

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

std::vector<char> readall(SOCKET s, sock_view addr)
{
    std::vector<char> bufsize;
    bufsize.resize(10000);

    int len = recvfrom(s, bufsize.data(), bufsize.size(), 0, (sockaddr*)addr.addr, &addr.len);

    assert(len != -1);

    assert(len <= (int)bufsize.size());

    bufsize.resize(len);

    return bufsize;
}

struct sock
{
    SOCKET s = INVALID_SOCKET;
    addrinfo* found_addr = nullptr;
    bool broadcast = false;

    sock(const std::string& address, const std::string& port, bool _broadcast) : broadcast(_broadcast)
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

        if(broadcast)
            hints.ai_flags = AI_PASSIVE;

        const char* node = broadcast ? nullptr : address.c_str();

        addrinfo* addr = nullptr;

        if(int result = getaddrinfo(node, port.c_str(), &hints, &addr); result != 0)
        {
            printf("Error at socket(): %d\n", WSAGetLastError());
            WSACleanup();
            throw std::runtime_error("Sock Error");
        }

        bool any = false;

        for(found_addr = addr; found_addr != nullptr; found_addr = found_addr->ai_next)
        {
            if(s = socket(found_addr->ai_family, found_addr->ai_socktype, found_addr->ai_protocol); s == (uint32_t)SOCKET_ERROR)
                continue;

            int yes = 1;

            int size = 1024 * 1024 * 5;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(int));
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(int));

            if(broadcast)
            {
                assert(setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(int)) != SOCKET_ERROR);

                if(bind(s, (sockaddr*)found_addr->ai_addr, found_addr->ai_addrlen) != -1)
                    any = true;
                else
                    continue;
            }

            break;
        }

        if(broadcast)
            assert(any);

        assert(s);
    }

    void write(const std::vector<char>& data)
    {
        assert(!broadcast);

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

        return readall(s, store);
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
        data_sock2 = new sock("127.255.255.255", "6960", true);

    return data_sock2;
}

sock* get_query_sock()
{
    if(query_sock2 == nullptr)
        query_sock2 = new sock("127.0.0.1", "6961", false);

    return query_sock2;
}

void data_write(char type, auto what)
{
    std::vector<char> data;
    data.push_back(type);

    add(data, what);

    get_query_sock()->write(data);
}

std::vector<char> query_read(char type)
{
    get_query_sock()->write(std::vector<char>{type});

    return get_query_sock()->read();
}

uint32_t DLL_EXPORT rtlsdr_get_device_count(void)
{
    LOG("Device Count");

    return 1;
}

const char* DLL_EXPORT rtlsdr_get_device_name(uint32_t index)
{
    LOG("Device name");

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
    LOG("Device usb strings");

    return rtlsdr_get_usb_strings(nullptr, m, p, s);
}

int DLL_EXPORT rtlsdr_get_index_by_serial(const char* serial)
{
    LOG("Idx By Serial");

    return 0;
}

char data_storage[1024] = {};

DLL_EXPORT int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index)
{
    LOG("Open");

    dev[0] = (rtlsdr_dev_t*)data_storage;
    return 0;
}

DLL_EXPORT int rtlsdr_close(rtlsdr_dev_t *dev)
{
    LOG("Close");

    return 0;
}

DLL_EXPORT int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
    LOG("Setx");

    return 0;
}

DLL_EXPORT int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
    LOG("Getx");

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
    LOG("usbs");

    if(m)
    {
        for(int i=0; i < 256; i++)
            m[i] = 0;
    }

    if(p)
    {
        for(int i=0; i < 256; i++)
            p[i] = 0;
    }

    if(s)
    {
        for(int i=0; i < 256; i++)
            s[i] = 0;
    }

    auto d_m = query_read(0x24);
    auto d_p = query_read(0x25);
    auto d_s = query_read(0x26);

    std::string s_m(d_m.begin(), d_m.end());
    std::string s_p(d_p.begin(), d_p.end());
    std::string s_s(d_s.begin(), d_s.end());

    if(s_m.size() > 255)
        s_m.resize(255);

    if(s_p.size() > 255)
        s_p.resize(255);

    if(s_s.size() > 255)
        s_s.resize(255);

    if(m)
        memcpy(m, s_m.data(), s_m.size());
    if(p)
        memcpy(p, s_p.data(), s_p.size());
    if(s)
        memcpy(s, s_s.data(), s_s.size());

    return 0;
}

DLL_EXPORT int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t* data, uint8_t offset, uint16_t len)
{
    LOG("w/e");

    return 0;
}

DLL_EXPORT int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
    LOG("r/e");

    return -3;
}

DLL_EXPORT int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
    LOG("setcq");

    data_write(0x01, freq);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
    LOG("getcq");

    auto result = query_read(0x20);

    return read_pop<uint32_t>(result).value();
}

DLL_EXPORT int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
    LOG("fsetfq");

    data_write(0x05, ppm);

    return 0;
}

DLL_EXPORT int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
    LOG("getf");

    auto result = query_read(0x12);

    return read_pop<int>(result).value();
}

DLL_EXPORT enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
    LOG("gettt");

    return RTLSDR_TUNER_R828D;
}

DLL_EXPORT int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
    LOG("gettg");

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
    LOG("settg");

    data_write(0x04, gain);
    return 0;
}

DLL_EXPORT int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw)
{
    LOG("settb");

    data_write(0x21, bw);

    return 0;
}

DLL_EXPORT int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
    LOG("gettg");

    auto data = query_read(0x15);

    return read_pop<int>(data).value();
}

DLL_EXPORT int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain)
{
    LOG("setifg");

    return 0;
}

DLL_EXPORT int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual)
{
    LOG("settgm");

    data_write(0x03, manual);

    return 0;
}

DLL_EXPORT int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate)
{
    LOG("setsr");

    data_write(0x02, rate);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
    LOG("getsr");

    auto data = query_read(0x16);

    return read_pop<uint32_t>(data).value();
}

DLL_EXPORT int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on)
{
    LOG("settest");

    return 0;
}

DLL_EXPORT int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on)
{
    LOG("agc");

    data_write(0x08, on);

    return 0;
}

DLL_EXPORT int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
    LOG("sds");

    data_write(0x09, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
    LOG("getds");

    auto dat = query_read(0x17);
    return read_pop<int>(dat).value();
}

DLL_EXPORT int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
    LOG("sot");

    data_write(0x0a, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
    LOG("got");

    auto dat = query_read(0x18);
    return read_pop<int>(dat).value();
}

DLL_EXPORT int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
    LOG("reset");

    return 0;
}

DLL_EXPORT int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
    LOG("reads");

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
    LOG("waita");

    return rtlsdr_read_async(dev, cb, ctx, 0, 0);
}

std::mutex mut;
bool has_ever_asked_for_data = false;

DLL_EXPORT int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len)
{
    LOG("reada");

    cancelled = 0;

    std::vector<unsigned char> next_data;
    int pop_size = 65536/4;

    {
        std::lock_guard guard(mut);

        if(!has_ever_asked_for_data)
        {
            has_ever_asked_for_data = true;
            get_query_sock()->write(std::vector<char>{0x0f});
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
    LOG("cancel");

    cancelled = 1;
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
    LOG("settee");

    data_write(0x0e, on);
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on)
{
    LOG("settee2");

    return 0;
}
