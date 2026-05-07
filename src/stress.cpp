#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <latch>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

static std::atomic<size_t> g_success{0};
static std::atomic<size_t> g_fail{0};

static const char REQUEST[] =
    "POST / HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Content-Type: application/json\r\n"
    "\r\n"
    "{\"command\": \"echo ok\"}";

static bool do_request(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd); return false;
    }

    write(fd, REQUEST, sizeof(REQUEST) - 1);

    char buf[512] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && strstr(buf, "200 OK");
}

static void client_thread(size_t n_reqs, const char* host, int port,
                           std::latch& ready) {
    ready.arrive_and_wait();
    for (size_t i = 0; i < n_reqs; ++i) {
        if (do_request(host, port))
            g_success.fetch_add(1, std::memory_order_relaxed);
        else
            g_fail.fetch_add(1, std::memory_order_relaxed);
    }
}

static void run_scenario(const char* label, size_t n_clients, size_t n_reqs,
                          const char* host, int port) {
    g_success = 0;
    g_fail    = 0;

    std::latch ready(static_cast<std::ptrdiff_t>(n_clients) + 1);
    std::vector<std::thread> threads;
    threads.reserve(n_clients);
    for (size_t i = 0; i < n_clients; ++i)
        threads.emplace_back(client_thread, n_reqs, host, port, std::ref(ready));

    ready.arrive_and_wait();
    auto t0 = Clock::now();
    for (auto& t : threads) t.join();
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    size_t total = g_success + g_fail;
    std::cout << std::left  << std::setw(45) << label
              << std::right << std::setw(6)  << total        << " reqs  "
              << std::setw(7) << g_success   << " ok  "
              << std::setw(7) << g_fail      << " fail  "
              << std::fixed  << std::setprecision(2)
              << std::setw(8) << (total / secs) << " req/s\n";
}

int main(int argc, char* argv[]) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int         port = argc > 2 ? atoi(argv[2]) : 8080;

    std::cout << "Stress test → " << host << ":" << port << "\n"
              << std::string(90, '-') << "\n"
              << std::left  << std::setw(45) << "Scenario"
              << std::right << std::setw(6)  << "Total"   << "       "
              << std::setw(7) << "OK"        << "     "
              << std::setw(7) << "Fail"      << "     "
              << "Throughput\n"
              << std::string(90, '-') << "\n";

    // Scenario A: clients <= workers (N_WORKERS=8), each makes 1 request
    run_scenario("A: 4 clients × 1 req  (<=workers, single-shot)",  4,   1, host, port);
    run_scenario("B: 8 clients × 1 req  (==workers, single-shot)",  8,   1, host, port);

    // Scenario B: clients >> workers, single request each
    run_scenario("C: 32 clients × 1 req  (>>workers, single-shot)", 32,  1, host, port);
    run_scenario("D: 64 clients × 1 req  (>>workers, single-shot)", 64,  1, host, port);

    // Scenario C: persistent-ish — each client sends 50 requests (new conn per req)
    run_scenario("E: 8  clients × 50 reqs (==workers, multi-req)",  8,  50, host, port);
    run_scenario("F: 32 clients × 50 reqs (>>workers, multi-req)", 32,  50, host, port);

    // Heavy: 100 clients × 100 requests
    run_scenario("G: 100 clients × 100 reqs (heavy load)",        100, 100, host, port);

    return 0;
}
