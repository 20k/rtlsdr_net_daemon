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

struct context
{
    sockaddr_storage whomst = {};
    SOCKET sock = {};
};

struct expiring_buffer
{
    std::chrono::time_point<std::chrono::steady_clock> when;
    std::vector<uint8_t> data;
    uint64_t id = 0;

    bool expired() const
    {
        auto now = std::chrono::steady_clock::now();

        return std::chrono::duration_cast<std::chrono::milliseconds>(now - when).count() > 2000;
    }
};

struct global_queue
{
    static inline std::atomic_uint64_t next_id = 0;

    std::vector<expiring_buffer> buffers;
    std::mutex mut;

    void add_buffer(const std::vector<uint8_t>& data)
    {
        std::lock_guard lock(mut);

        while(buffers.size() > 0 && buffers.front().expired())
            buffers.erase(buffers.begin());

        expiring_buffer exp;
        exp.when = std::chrono::steady_clock::now();
        exp.data = data;
        exp.id = next_id++;

        buffers.push_back(std::move(exp));
    }

    std::vector<expiring_buffer> get_buffers_after(uint64_t id)
    {
        std::lock_guard lock(mut);

        std::vector<expiring_buffer> ret;

        for(auto& i : buffers)
        {
            if(i.id >= id)
                ret.push_back(i);
        }

        return ret;
    }
};

global_queue gqueue;

void pipe_data_into_queue(unsigned char* buf, uint32_t len, void* ctx)
{
    std::vector<uint8_t> data;
    data.resize(len);

    memcpy(data.data(), buf, len);

    gqueue.add_buffer(data);
}

bool sendall(SOCKET s, sockaddr_storage addr, const std::vector<char>& data)
{
    int64_t bytes_sent = 0;

    while(bytes_sent < data.size())
    {
        int count = sendto(s, data.data() + bytes_sent, data.size() - bytes_sent, 0, (sockaddr*)&addr, sizeof(addr));

        if(count == -1)
            return true;

        bytes_sent += count;
    }

    return false;
}

void async_thread(context* ctx)
{
    uint64_t last_id = gqueue.next_id;

    while(1)
    {
        if(global_close)
            break;

        auto to_write = gqueue.get_buffers_after(last_id);

        if(to_write.size() > 0)
        {
            last_id = to_write.back().id;

            for(expiring_buffer& buf : to_write)
            {
                int chunk_size = 1024;

                for(int i=0; i < buf.data.size(); i += chunk_size)
                {
                    int fin = std::min(i + chunk_size, (int)buf.data.size());

                    std::vector<char> ldat(buf.data.begin() + i, buf.data.begin() + fin);

                    if(sendall(ctx->sock, ctx->whomst, ldat))
                        return;
                }
            }
        }
        else
        {
            sf::sleep(sf::milliseconds(1));
        }
    }
}

struct sock
{
    SOCKET listen_sock = INVALID_SOCKET;

    sock(const std::string& port)
    {
        WSADATA wsa_data;

        if(auto result = WSAStartup(MAKEWORD(2,2), &wsa_data); result != 0)
        {
            printf("WSAStartup failed: %d\n", result);
            assert(false);
        }

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


        listen_sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

        if(listen_sock == INVALID_SOCKET)
        {
            printf("Error at socket(): %ld\n", WSAGetLastError());
            freeaddrinfo(addr);
            WSACleanup();
            assert(false);
        }

        int yes = 1;

        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(int));

        if(auto result = bind(listen_sock, addr->ai_addr, (int)addr->ai_addrlen); result == SOCKET_ERROR)
        {
            printf("bind failed with error: %d\n", WSAGetLastError());
            freeaddrinfo(addr);
            closesocket(listen_sock);
            WSACleanup();
            assert(false);
        }
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
            printf("Error receiving from anyone\n");
            return {};
        }

        return {data, from};
    }

    void send_all(sockaddr_storage to, const std::vector<char>& data)
    {
        sendall(listen_sock, to, data);
    }

    void send_all(sockaddr_storage to, unsigned int in)
    {
        std::vector<char> data;
        data.resize(sizeof(in));

        memcpy(data.data(), &in, sizeof(in));

        return send_all(to, data);
    }

    void send_all(sockaddr_storage to, const std::string& in)
    {
        std::vector<char> data(in.begin(), in.end());

        return send_all(to, data);
    }
};

void add(std::vector<char>& in, int v)
{
    auto len = in.size();

    in.resize(in.size() + sizeof(int));

    memcpy(in.data() + len, &v, sizeof(int));
}

void add(std::vector<char>& in, const std::vector<int> v)
{
    for(auto& i : v)
        add(in, i);
}

int main()
{
    device dev;

    dev.set_freq(1000000);

    std::jthread([&]()
    {
        rtlsdr_read_async(dev.v, pipe_data_into_queue, nullptr, 0, 0);
    }).detach();

    sock sck("6960");
    sock info("6961");

    std::jthread([&]()
    {
        while(1)
        {
            auto [data, from] = info.read_all();

            if(data.size() < 1)
                continue;

            unsigned char cmd = data[0];

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
        }

    }).detach();

    while(1)
    {
        auto [data, from] = sck.read_all();

        if(data.size() < sizeof(char) + sizeof(int))
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
            //ctx->dv = dev.v;
            ctx->whomst = from;
            ctx->sock = sck.listen_sock;

            std::jthread([](context* ctx)
            {
                async_thread(ctx);
            }, ctx).detach();
        }
    }

    global_close = 1;
    rtlsdr_cancel_async(dev.v);

    return 0;
}
