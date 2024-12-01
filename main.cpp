#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <rtl-sdr.h>
#include <assert.h>
#include <thread>
#include <chrono>
#include <SFML/System/Sleep.hpp>

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

struct global_queue
{
    std::vector<std::vector<uint8_t>> buffers;
    std::mutex mut;

    void add_buffer(std::vector<uint8_t>&& data)
    {
        std::lock_guard lock(mut);

        buffers.emplace_back(std::move(data));
    }

    std::vector<std::vector<uint8_t>> pop_buffers()
    {
        std::lock_guard lock(mut);

        std::vector<std::vector<uint8_t>> ret = std::move(buffers);
        buffers.clear();
        return ret;
    }
};

global_queue gqueue;

void pipe_data_into_queue(unsigned char* buf, uint32_t len, void* ctx)
{
    std::vector<uint8_t> data;
    data.resize(len);

    memcpy(data.data(), buf, len);

    gqueue.add_buffer(std::move(data));
}

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

bool sendall(SOCKET s, sock_view sv, const std::vector<char>& data)
{
    int64_t bytes_sent = 0;

    while(bytes_sent < data.size())
    {
        int count = sendto(s, data.data() + bytes_sent, data.size() - bytes_sent, 0, sv.addr, sv.len);

        if(count == -1)
            return true;

        bytes_sent += count;
    }

    return false;
}

struct sock
{
    SOCKET listen_sock = INVALID_SOCKET;
    addrinfo* broadcast_address = nullptr;

    sock(const std::string& port, bool broadcast)
    {
        #ifdef _WIN32
        WSADATA wsa_data;

        if(auto result = WSAStartup(MAKEWORD(2,2), &wsa_data); result != 0)
        {
            printf("WSAStartup failed: %d\n", result);
            assert(false);
        }
        #endif

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* addr = nullptr;

        if(auto result = getaddrinfo(nullptr, port.c_str(), &hints, &addr); result != 0)
        {
            printf("getaddrinfo failed: %d\n", result);
            WSACleanup();
            assert(false);
        }

        addrinfo bc_hints = {};
        bc_hints.ai_family = AF_INET;
        bc_hints.ai_socktype = SOCK_DGRAM;
        bc_hints.ai_protocol = IPPROTO_UDP;

        if(auto result = getaddrinfo("127.255.255.255", port.c_str(), &bc_hints, &broadcast_address))
        {
            printf("getaddrinfo2 failed: %d\n", result);
            WSACleanup();
            assert(false);
        }

        listen_sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

        if(listen_sock == INVALID_SOCKET)
        {
            printf("Error at socket(): %d\n", WSAGetLastError());
            freeaddrinfo(addr);
            WSACleanup();
            assert(false);
        }

        int yes = 1;
        int size = 1024 * 1024 * 5;

        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(int));
        setsockopt(listen_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(int));
        setsockopt(listen_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(int));

        if(auto result = bind(listen_sock, addr->ai_addr, (int)addr->ai_addrlen); result == SOCKET_ERROR)
        {
            printf("bind failed with error: %d\n", WSAGetLastError());
            freeaddrinfo(addr);
            closesocket(listen_sock);
            WSACleanup();
            assert(false);
        }

        #ifdef _WIN32
        #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)

        bool report_errors = false;
        DWORD bytes = 0;
        WSAIoctl(listen_sock, SIO_UDP_CONNRESET, &report_errors, sizeof(report_errors), nullptr, 0, &bytes, nullptr, nullptr);
        #endif
    }

    bool can_read()
    {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(listen_sock, &reads);

        struct timeval tv = {};

        select(listen_sock+1, &reads, nullptr, nullptr, &tv);

        return FD_ISSET(listen_sock, &reads);
    }

    std::pair<std::vector<char>, sockaddr_storage> read_all()
    {
        std::vector<char> data;
        data.resize(1024);

        sockaddr_storage from = {};
        int sock_size = sizeof(from);

        int64_t numbytes = 0;

        if(numbytes = recvfrom(listen_sock, data.data(), data.size(), 0, (sockaddr*)&from, &sock_size); numbytes == -1)
        {
            printf("Error receiving from anyone %d\n", WSAGetLastError());
            return {};
        }

        return {data, from};
    }

    void send_all(sock_view sv, const std::vector<char>& data)
    {
        sendall(listen_sock, sv, data);
    }

    void send_all(sock_view sv, unsigned int in)
    {
        std::vector<char> data;
        data.resize(sizeof(in));

        memcpy(data.data(), &in, sizeof(in));

        return send_all(sv, data);
    }

    void send_all(sock_view sv, const std::string& in)
    {
        std::vector<char> data(in.begin(), in.end());

        return send_all(sv, data);
    }

    void broadcast(const auto& what)
    {
        return send_all(broadcast_address, what);
    }
};

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

///todo: multiple devices
int main()
{
    device dev;

    dev.set_freq(1000000);

    std::jthread([&]()
    {
        rtlsdr_read_async(dev.v, pipe_data_into_queue, nullptr, 0, 0);
    }).detach();

    sock sck("6960", true);
    sock info("6961", false);

    std::jthread([&]()
    {
        while(1)
        {
            if(global_close)
                break;

            auto to_write = gqueue.pop_buffers();

            if(to_write.size() == 0)
            {
                sf::sleep(sf::milliseconds(1));
                continue;
            }

            for(std::vector<uint8_t>& buf : to_write)
            {
                int chunk_size = 1024*8;

                for(int i=0; i < buf.size(); i += chunk_size)
                {
                    int fin = std::min(i + chunk_size, (int)buf.size());

                    int num = sendto(sck.listen_sock, (const char*)(buf.data() + i), fin - i, 0, (sockaddr*)sck.broadcast_address->ai_addr, sck.broadcast_address->ai_addrlen);

                    if(num == -1)
                        return;

                    assert(num == (fin - i));
                }
            }

        }
    }).detach();

    while(1)
    {
        std::vector<std::pair<std::vector<char>, sockaddr_storage>> all_dat;

        while(info.can_read())
        {
            all_dat.push_back(info.read_all());
        }

        auto count_type = [&](unsigned char type)
        {
            int counts = 0;

            for(auto& [a, b] : all_dat)
            {
                if(a.size() == 0)
                    continue;

                if(a[0] == type)
                    counts++;
            }

            return counts;
        };

        auto remove_num_of = [&](unsigned char type, int num)
        {
            if(num <= 0)
                return;

            int counts = 0;

            for(int i=0; i < (int)all_dat.size(); i++)
            {
                if(all_dat[i].first.size() == 0)
                    continue;

                if(all_dat[i].first[0] == type)
                {
                    all_dat.erase(all_dat.begin() + i);
                    i--;
                    counts++;

                    if(counts == num)
                        return;
                }
            }
        };

        auto remove_all_except_last = [&](unsigned char type)
        {
            auto cnt = count_type(type);
            remove_num_of(type, cnt - 1);
        };

        remove_all_except_last(0x01);
        remove_all_except_last(0x02);
        remove_all_except_last(0x03);
        remove_all_except_last(0x04);
        remove_all_except_last(0x05);
        remove_all_except_last(0x0d);
        remove_all_except_last(0x21);

        if(all_dat.size() == 0)
            sf::sleep(sf::milliseconds(1));

        for(auto& [data, from] : all_dat)
        {
            if(data.size() < 1)
                continue;

            unsigned char cmd = data[0];

            printf("Qcmd %i\n", cmd);

            if(cmd == 0x10)
            {
                info.send_all(from, rtlsdr_get_device_count());
            }

            if(cmd == 0x11 && data.size() >= 5)
            {
                uint32_t idx = 0;
                memcpy(&idx, data.data() + 1, sizeof(idx));

                std::string name(rtlsdr_get_device_name(idx));

                info.send_all(from, name);
            }

            if(cmd == 0x12)
            {
                info.send_all(from, rtlsdr_get_freq_correction(dev.v));
            }

            if(cmd == 0x13)
            {
                info.send_all(from, rtlsdr_get_tuner_type(dev.v));
            }

            if(cmd == 0x14)
            {
                std::vector<int> gains = dev.get_gains();

                std::vector<char> dat;
                add(dat, (int)gains.size());
                add(dat, gains);

                info.send_all(from, dat);
            }

            if(cmd == 0x15)
            {
                int gain = rtlsdr_get_tuner_gain(dev.v);

                info.send_all(from, gain);
            }

            if(cmd == 0x16)
            {
                uint32_t rate = rtlsdr_get_sample_rate(dev.v);

                info.send_all(from, rate);
            }

            if(cmd == 0x17)
            {
                uint32_t sam = rtlsdr_get_direct_sampling(dev.v);

                info.send_all(from, sam);
            }

            if(cmd == 0x18)
            {
                uint32_t t = rtlsdr_get_offset_tuning(dev.v);

                info.send_all(from, t);
            }

            if(cmd == 0x19)
            {
                uint32_t freq1 = {};
                uint32_t freq2 = {};
                rtlsdr_get_xtal_freq(dev.v, &freq1, &freq2);

                std::vector<char> dat;
                add(dat, freq1);
                add(dat, freq2);

                info.send_all(from, dat);
            }

            if(cmd == 0x20)
            {
                uint32_t freq = rtlsdr_get_center_freq(dev.v);

                info.send_all(from, freq);
            }

            if(cmd == 0x24)
            {
                char m[257] = {};
                rtlsdr_get_usb_strings(dev.v, m, nullptr, nullptr);

                info.send_all(from, std::string(m));
            }
            if(cmd == 0x25)
            {
                char p[257] = {};
                rtlsdr_get_usb_strings(dev.v, nullptr, p, nullptr);

                info.send_all(from, std::string(p));
            }
            if(cmd == 0x26)
            {
                char s[257] = {};
                rtlsdr_get_usb_strings(dev.v, nullptr, nullptr, s);

                info.send_all(from, std::string(s));
            }

            /*if(cmd == 0x0f)
            {
                context* ctx = new context;
                //ctx->dv = dev.v;
                ctx->whomst = from;
                ctx->sock = sck.listen_sock;

                std::jthread([](context* ctx)
                {
                    async_thread(ctx);
                }, ctx).detach();
            }*/

            if(data.size() < sizeof(char) + sizeof(int))
                continue;

            unsigned int param = 0;
            memcpy((void*)&param, (void*)&data[1], sizeof(int));

            printf("Got wcmd %i\n", cmd);

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
            if(cmd == 0x21)
                rtlsdr_set_tuner_bandwidth(dev.v, param);
        }
    }

    global_close = 1;
    rtlsdr_cancel_async(dev.v);

    return 0;
}
