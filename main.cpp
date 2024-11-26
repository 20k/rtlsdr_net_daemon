#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <rtl-sdr.h>
#include <assert.h>
#include <thread>

std::atomic_int global_close{0};

struct device
{
    rtlsdr_dev_t* v = nullptr;
    std::vector<int> gains_intl;

    device()
    {
        assert(rtlsdr_open(&v, 0) == 0);

        rtlsdr_set_sample_rate(v, 2400000);

        int gains[128] = {};
        int ngain = rtlsdr_get_tuner_gains(v, gains);

        for(int i=0; i < ngain; i++)
        {
            printf("%i ", gains[i]);
        }

        gains_intl.resize(ngain);

        for(int i=0; i < ngain && i < 128; i++)
            gains_intl[i] = gains[i];

        rtlsdr_set_tuner_gain(v, gains[0]);

        rtlsdr_reset_buffer(v);
    }

    ~device()
    {
        assert(rtlsdr_close(v) == 0);
    }

    std::vector<int> get_gains()
    {
        return gains_intl;
    }

    void set_gain(int gain)
    {
        for(auto& i : gains_intl)
        {
            if(i == gain)
            {
                rtlsdr_set_tuner_gain(v, gain);
                return;
            }
        }

        return;
    }

    void set_bandwidth(uint32_t width)
    {
        rtlsdr_set_tuner_bandwidth(v, width);
    }

    void set_freq(uint32_t hz)
    {
        rtlsdr_set_center_freq(v, hz);
    }

    uint32_t get_freq()
    {
        return rtlsdr_get_center_freq(v);
    }
};

struct context
{
    rtlsdr_dev_t* dv = nullptr;
    sockaddr_storage whomst = {};
};

void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx)
{

}

void async_thread(context* ctx)
{
    while(1)
    {
        if(global_close)
            break;
    }
}

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

    device dev;

    dev.set_freq(1000000);

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

        if(numbytes < sizeof(char) + sizeof(int))
            continue;

        unsigned char cmd = data[0];
        unsigned int param = 0;
        memcpy((void*)&param, (void*)&data[1], sizeof(int));

        if(cmd == 0x01)
            rtlsdr_set_center_freq(dev.v, param);
        if(cmd == 0x02)
            rtlsdr_set_sample_rate(dev.v, param);
        if(cmd == 0x03)
            rtlsdr_set_tuner_gain_mode(dev.v, param);
        if(cmd == 0x04)
            rtlsdr_set_tuner_gain(dev.v, param);
        if(cmd == 0x05)
            rtlsdr_set_freq_correction(dev.v, param);
        if(cmd == 0x06)
        {}
        if(cmd == 0x07)
            rtlsdr_set_testmode(dev.v, param);
        if(cmd == 0x08)
            rtlsdr_set_agc_mode(dev.v, param);
        if(cmd == 0x09)
            rtlsdr_set_direct_sampling(dev.v, param);
        if(cmd == 0x0a)
            rtlsdr_set_offset_tuning(dev.v, param);
        if(cmd == 0x0b)
            rtlsdr_set_xtal_freq(dev.v, param, 0);
        if(cmd == 0x0c)
            rtlsdr_set_xtal_freq(dev.v, 0, param);
        if(cmd == 0x0d)
            dev.set_gain(param);
        if(cmd == 0x0e)
            rtlsdr_set_bias_tee(dev.v, param);

        if(cmd == 0x0f)
        {
            context* ctx = new context;
            ctx->dv = dev.v;
            ctx->whomst = from;

            std::jthread([](context* ctx)
            {
                async_thread(ctx);
            }, ctx).detach();

            //sockaddr_in* as_addr = (sockaddr_in*)&from;

            //as_addr->
        }
    }

    global_close = 1;

    return 0;
}
