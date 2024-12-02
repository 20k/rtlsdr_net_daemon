#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <thread>
#include <math.h>

#define DLL_EXPORT __declspec(dllexport)

FILE* get_file()
{
    static FILE* file = fopen("./dump.txt", "a");
    return file;
}

//#define LOG(x) fwrite(x"\n", strlen(x"\n"), 1, get_file())

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

void sendall(SOCKET s, sock_view sv, const std::span<const char>& data)
{
    int len = sendto(s, data.data(), data.size(), 0, sv.addr, sv.len);

    assert(len != -1);
    assert(len == (int)data.size());
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

            #ifdef _WIN32
            #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)

            bool report_errors = false;
            DWORD bytes = 0;
            WSAIoctl(s, SIO_UDP_CONNRESET, &report_errors, sizeof(report_errors), nullptr, 0, &bytes, nullptr, nullptr);
            #endif

            break;
        }

        if(broadcast)
            assert(any);

        assert(s);
    }

    void stop()
    {
        #ifdef _WIN32
        closesocket(s);
        #else
        close(s);
        #endif
    }

    void write(const std::vector<char>& data)
    {
        assert(!broadcast);

        sendall(s, found_addr, data);
    }

    void write(unsigned int in)
    {
        std::vector<char> data;
        data.resize(sizeof(in));

        memcpy(data.data(), &in, sizeof(in));

        return write(data);
    }

    bool can_read(double timeout_s = 0)
    {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(s, &reads);

        struct timeval tv = {};

        tv.tv_sec = timeout_s;
        tv.tv_usec = (timeout_s - floor(timeout_s)) * 1000. * 1000.;

        select(s+1, &reads, nullptr, nullptr, &tv);

        return FD_ISSET(s, &reads);
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

sock* query_sock2 = nullptr;

sock* get_query_sock()
{
    if(query_sock2 == nullptr)
        query_sock2 = new sock("127.0.0.1", "6961", false);

    return query_sock2;
}

struct context
{
    uint32_t index = 0;
    std::string port;

    std::atomic_bool cancelled{false};

    std::mutex mut;
    std::vector<char> data_queue;

    std::jthread data_thread;
    uint32_t max_bytes = 1024 * 1024 * 10;
    std::atomic_bool has_data_queue{false};

    void start_data_queue()
    {
        has_data_queue = true;

        data_thread = std::jthread([&](std::stop_token tok)
        {
            sock sck("127.255.255.255", port, true);

            while(!tok.stop_requested())
            {
                while(sck.can_read() && !tok.stop_requested())
                {
                    std::vector<char> data = sck.read();

                    std::lock_guard guard(mut);

                    data_queue.insert(data_queue.end(), data.begin(), data.end());

                    if(data_queue.size() > max_bytes)
                    {
                        uint32_t extra = data_queue.size() - max_bytes;

                        data_queue = std::vector<char>(data_queue.begin() + extra, data_queue.end());
                    }
                }

                sf::sleep(sf::milliseconds(1));
            }

            {
                std::lock_guard guard(mut);
                data_queue.clear();
            }

            sck.stop();
        });
    }

    uint32_t data_queue_size()
    {
        std::lock_guard guard(mut);
        return data_queue.size();
    }

    void clear_data_queue()
    {
        stop_data_queue();
        start_data_queue();
    }

    std::vector<char> pop_data_queue()
    {
        std::vector<char> ret;

        std::lock_guard guard(mut);

        ret = std::move(data_queue);
        data_queue.clear();

        return ret;
    }

    void stop_data_queue()
    {
        if(!has_data_queue)
            return;

        data_thread.request_stop();
        data_thread.join();

        has_data_queue = false;
    }
};

void data_write(rtlsdr_dev_t* rctx, char type, auto what)
{
    assert(rctx);

    context* ctx = (context*)rctx;

    std::vector<char> data;
    data.push_back(type);

    add(data, ctx->index);
    add(data, what);

    get_query_sock()->write(data);
}

///do a basic retry loop on a timeout, to guarantee success if the underlying
std::vector<char> write_read_loop(const std::vector<char>& to_write)
{
    bool any_data = false;

    while(!any_data)
    {
        get_query_sock()->write(to_write);

        any_data = get_query_sock()->can_read(10);
    }

    return get_query_sock()->read();
}

std::vector<char> query_read_eidx(uint32_t index, char type)
{
    std::vector<char> to_write{type};

    add(to_write, index);

    return write_read_loop(to_write);
}

std::vector<char> query_read(rtlsdr_dev_t* rctx, char type)
{
    assert(rctx);

    context* ctx = (context*)rctx;

    return query_read_eidx(ctx->index, type);
}

uint32_t DLL_EXPORT rtlsdr_get_device_count(void)
{
    LOG("Device Count");

    std::vector<char> to_write{0x10};

    auto data = write_read_loop(to_write);

    return read_pop<int>(data).value();
}

const char* DLL_EXPORT rtlsdr_get_device_name(uint32_t index)
{
    LOG("Device name");

    auto data = query_read_eidx(index, 0x11);

    std::string* leaked = new std::string(data.begin(), data.end());

    return leaked->c_str();
}

int DLL_EXPORT rtlsdr_get_device_usb_strings(uint32_t index,
					     char* m,
					     char* p,
					     char* s)
{
    LOG("Device usb strings");

    if(m)
        memset(m, 0, 256);

    if(p)
        memset(p, 0, 256);

    if(s)
        memset(s, 0, 256);

    auto d_m = query_read_eidx(index, 0x24);
    auto d_p = query_read_eidx(index, 0x25);
    auto d_s = query_read_eidx(index, 0x26);

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

int DLL_EXPORT rtlsdr_get_index_by_serial(const char* serial)
{
    LOG("Idx By Serial");

	if(serial == nullptr)
		return -1;

	uint32_t count = rtlsdr_get_device_count();

	if(count == 0)
		return -2;

    std::string sserial(serial);

	for(uint32_t i = 0; i < count; i++)
    {
        char s[257] = {};

		rtlsdr_get_device_usb_strings(i, nullptr, nullptr, s);

		if(sserial == std::string(s))
			return i;
	}

	return -3;
}

DLL_EXPORT int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index)
{
    LOG("Open");

    context* out = new context();
    out->index = index;

    auto data = query_read_eidx(index, 0x27);

    uint32_t port = read_pop<uint32_t>(data).value();

    out->port = std::to_string(port);
    out->start_data_queue();

    dev[0] = (rtlsdr_dev_t*)out;
    return 0;
}

DLL_EXPORT int rtlsdr_close(rtlsdr_dev_t *dev)
{
    LOG("Close");

    if(dev == nullptr)
        return -1;

    context* ctx = (context*)dev;

    ctx->stop_data_queue();

    delete ctx;

    return 0;
}

DLL_EXPORT int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
    LOG("Setx");

    data_write(dev, 0x0b, rtl_freq);
    data_write(dev, 0x0c, tuner_freq);

    return 0;
}

DLL_EXPORT int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
    LOG("Getx");

    std::vector<char> read = query_read(dev, 0x19);

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

    assert(dev);

    return rtlsdr_get_device_usb_strings(((context*)dev)->index, m, p, s);
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

    data_write(dev, 0x01, freq);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
    LOG("getcq");

    auto result = query_read(dev, 0x20);

    return read_pop<uint32_t>(result).value();
}

DLL_EXPORT int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
    LOG("fsetfq");

    data_write(dev, 0x05, ppm);

    return 0;
}

DLL_EXPORT int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
    LOG("getf");

    auto result = query_read(dev, 0x12);

    return read_pop<int>(result).value();
}

///todo: dome
DLL_EXPORT enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
    LOG("gettt");

    auto result = query_read(dev, 0x13);

    return (enum rtlsdr_tuner)read_pop<int>(result).value();
}

DLL_EXPORT int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
    LOG("gettg");

    auto result = query_read(dev, 0x14);

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

    data_write(dev, 0x04, gain);
    return 0;
}

DLL_EXPORT int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw)
{
    LOG("settb");

    data_write(dev, 0x21, bw);

    return 0;
}

DLL_EXPORT int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
    LOG("gettg");

    auto data = query_read(dev, 0x15);

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

    data_write(dev, 0x03, manual);

    return 0;
}

DLL_EXPORT int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate)
{
    LOG("setsr");

    data_write(dev, 0x02, rate);

    return 0;
}

DLL_EXPORT uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
    LOG("getsr");

    auto data = query_read(dev, 0x16);

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

    data_write(dev, 0x08, on);

    return 0;
}

DLL_EXPORT int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
    LOG("sds");

    data_write(dev, 0x09, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
    LOG("getds");

    auto dat = query_read(dev, 0x17);
    return read_pop<int>(dat).value();
}

DLL_EXPORT int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
    LOG("sot");

    data_write(dev, 0x0a, on);
    return 0;
}

DLL_EXPORT int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
    LOG("got");

    auto dat = query_read(dev, 0x18);
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

    if(len < 0)
        return 0;

    assert(dev);

    context* ctx = (context*)dev;

    std::vector<char> data = ctx->pop_data_queue();

    assert(buf);

    while((uint32_t)data.size() < (uint32_t)len)
    {
        auto next = ctx->pop_data_queue();

        data.insert(data.end(), next.begin(), next.end());
    }

    if((int)data.size() > len)
    {
        uint32_t extra = (uint32_t)data.size() - (uint32_t)len;
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

DLL_EXPORT int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *user_ctx, uint32_t buf_num, uint32_t buf_len)
{
    LOG("reada");

    assert(dev);

    context* ctx = (context*)dev;

    ctx->cancelled = false;
    ctx->clear_data_queue();

    std::vector<unsigned char> next_data;
    uint32_t pop_size = 65536/4;

    if(buf_len > 0)
        buf_len = pop_size;

    while(!ctx->cancelled)
    {
        auto data = ctx->pop_data_queue();

        next_data.insert(next_data.end(), data.begin(), data.end());

        if((uint32_t)next_data.size() >= pop_size)
        {
            std::span<unsigned char> corrected(next_data.begin(), pop_size);

            cb((unsigned char*)corrected.data(), (uint32_t)corrected.size(), user_ctx);

            next_data = std::vector<unsigned char>(next_data.begin() + pop_size, next_data.end());
        }
        else
        {
            sf::sleep(sf::milliseconds(1));
        }
    }

    return 0;
}

DLL_EXPORT int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
    LOG("cancel");

    assert(dev);

    context* ctx = (context*)dev;

    ctx->cancelled = true;
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
    LOG("settee");

    data_write(dev, 0x0e, on);
    return 0;
}

DLL_EXPORT int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on)
{
    LOG("settee2");

    return 0;
}
