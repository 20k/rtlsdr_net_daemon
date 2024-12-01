#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <rtl-sdr.h>
#include <assert.h>
#include <thread>
#include <chrono>
#include <SFML/System/Sleep.hpp>
#include <set>

std::atomic_int global_close{0};

uint32_t device_count()
{
    return rtlsdr_get_device_count();
}

struct device
{
    rtlsdr_dev_t* v = nullptr;
    std::vector<int> gains_intl;

    device(uint32_t index)
    {
        assert(rtlsdr_open(&v, index) == 0);

        rtlsdr_set_sample_rate(v, 2400000);

        int ngain = rtlsdr_get_tuner_gains(v, nullptr);

        gains_intl.resize(ngain);

        ngain = rtlsdr_get_tuner_gains(v, gains_intl.data());

        assert(ngain == gains_intl.size());

        for(int i=0; i < gains_intl.size(); i++)
        {
            printf("%i ", gains_intl[i]);
        }

        printf("\n");

        rtlsdr_set_tuner_gain(v, gains_intl[0]);

        rtlsdr_reset_buffer(v);
    }

    device(device&& other)
    {
        v = other.v;
        gains_intl = other.gains_intl;

        other.v = nullptr;
    }

    device& operator=(device&& other)
    {
        v = other.v;
        gains_intl = other.gains_intl;

        other.v = nullptr;
        return *this;
    }

    ~device()
    {
        if(v)
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

struct async_context
{
    global_queue q;
    sock s;

    async_context(const std::string& port, bool broadcast) : s(port, broadcast)
    {

    }
};

void pipe_data_into_queue(unsigned char* buf, uint32_t len, void* ctx)
{
    async_context* actx = (async_context*)ctx;

    std::vector<uint8_t> data;
    data.resize(len);

    memcpy(data.data(), buf, len);

    actx->q.add_buffer(std::move(data));
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

struct data_format
{
    uint8_t cmd = 0;
    std::optional<uint32_t> index = 0;
    std::optional<uint32_t> param = 0;

    void load(const std::vector<char>& in)
    {
        if(in.size() == 0)
            return;

        if(in.size() >= 1)
            cmd = in[0];

        if(in.size() >= sizeof(char) + sizeof(uint32_t))
        {
            index = 0;
            memcpy(&index.value(), &in[1], sizeof(uint32_t));
        }

        if(in.size() >= sizeof(char) + sizeof(uint32_t)*2)
        {
            param = 0;
            memcpy(&param.value(), &in[5], sizeof(uint32_t));
        }
    }
};

///todo: multiple devices
///so: we send a query when we open a device, and that query should return the socket for the device
int main()
{

    std::vector<device> devs;

    uint32_t dcount = device_count();

    for(uint32_t i = 0; i < dcount; i++)
        devs.emplace_back(i);

    std::cout << "Found devices " << dcount << std::endl;

    auto device_to_port = [](uint32_t index) -> uint32_t
    {
        return 6962 + index;
    };

    std::vector<async_context*> contexts;

    for(int i=0; i < dcount; i++)
    {
        uint64_t root = device_to_port(i);

        async_context* actx = new async_context(std::to_string(root), true);

        contexts.push_back(actx);
    }

    for(auto& dev : devs)
        dev.set_freq(1000000);

    for(int i=0; i < dcount; i++)
    {
        device& dev = devs[i];
        async_context* actx = contexts[i];

        std::jthread([&]()
        {
            rtlsdr_read_async(dev.v, pipe_data_into_queue, actx, 0, 0);
        }).detach();
    }

    sock info("6961", false);

    std::jthread([&]()
    {
        while(1)
        {
            if(global_close)
                break;

            bool all_zero = true;

            for(async_context* actx : contexts)
            {
                auto to_write = actx->q.pop_buffers();

                if(to_write.size() == 0)
                    continue;

                all_zero = false;

                for(std::vector<uint8_t>& buf : to_write)
                {
                    int chunk_size = 1024*8;

                    for(int i=0; i < buf.size(); i += chunk_size)
                    {
                        int fin = std::min(i + chunk_size, (int)buf.size());

                        int num = sendto(actx->s.listen_sock, (const char*)(buf.data() + i), fin - i, 0, (sockaddr*)actx->s.broadcast_address->ai_addr, actx->s.broadcast_address->ai_addrlen);

                        if(num == -1)
                            return;

                        assert(num == (fin - i));
                    }
                }
            }

            if(all_zero)
                sf::sleep(sf::milliseconds(1));
        }
    }).detach();

    while(1)
    {
        std::vector<std::pair<data_format, sockaddr_storage>> all_dat;

        while(info.can_read())
        {
            auto [dat, storage] = info.read_all();

            data_format fmt;
            fmt.load(dat);

            all_dat.push_back({fmt, storage});
        }

        std::set<std::pair<uint32_t, uint32_t>> already_seen;

        std::set<uint32_t> permitted_to_elide = {
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x0d,
            0x21
        };

        for(int i=(int)all_dat.size() - 1; i >= 0; i--)
        {
            data_format& fmt = all_dat[i].first;

            if(!fmt.index)
                continue;

            if(permitted_to_elide.count(fmt.cmd) == 0)
                continue;

            std::pair<uint32_t, uint32_t> to_check = {fmt.cmd, fmt.index.value()};

            if(already_seen.count(to_check) == 0)
            {
                already_seen.insert(to_check);
                continue;
            }

            all_dat.erase(all_dat.begin() + i);
        }

        if(all_dat.size() == 0)
            sf::sleep(sf::milliseconds(1));

        for(auto& [fmt, from] : all_dat)
        {
            uint8_t cmd = fmt.cmd;

            printf("Qcmd %i\n", cmd);

            if(cmd == 0x10)
            {
                info.send_all(from, rtlsdr_get_device_count());
            }

            if(cmd == 0x11)
            {
                std::string name(rtlsdr_get_device_name(fmt.index.value_or(0)));

                info.send_all(from, name);
            }

            if(!fmt.index)
                continue;

            uint32_t device_index = fmt.index.value();

            if(device_index >= devs.size())
                continue;

            device& dev = devs.at(device_index);

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

            ///query port
            if(cmd == 0x27)
            {
                uint32_t port = device_to_port(device_index);

                if(device_index >= devs.size())
                    port = 0;

                info.send_all(from, port);
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

            if(!fmt.param)
                continue;

            uint32_t param = fmt.param.value();

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

    for(device& dev : devs)
        rtlsdr_cancel_async(dev.v);

    return 0;
}
