#include "packet_pool.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr int    PORT        = 8080;
static constexpr int    N_WORKERS   = 8;
static constexpr size_t BUFFER_SIZE = 4096;
static constexpr int    BACKLOG     = 256;
static constexpr int    MAX_EVENTS  = 64;

static const char* forbidden[] = {"rm", "chmod", "chown", "sudo", "shutdown", nullptr};

static bool is_forbidden(const char* cmd) {
    for (int i = 0; forbidden[i]; ++i)
        if (strstr(cmd, forbidden[i])) return true;
    return false;
}

static void execute_pipeline(char* cmd_str, int client_fd) {
    if (is_forbidden(cmd_str)) {
        dprintf(client_fd, "HTTP/1.1 403 Forbidden\r\n\r\nError: Command not allowed.\n");
        return;
    }

    char* cmds[10];
    char* saveptr;
    int n = 0;

    for (char* tok = strtok_r(cmd_str, "~", &saveptr); tok && n < 10;
         tok = strtok_r(nullptr, "~", &saveptr))
        cmds[n++] = tok;

    pid_t pids[10];
    int   npids   = 0;
    pid_t pgid    = 0;
    int   prev_fd = -1;

    for (int i = 0; i < n; ++i) {
        int pipe_fds[2] = {-1, -1};
        if (i < n - 1) pipe(pipe_fds);

        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, pgid ? pgid : 0);
            if (prev_fd != -1) { dup2(prev_fd, STDIN_FILENO);  close(prev_fd); }
            if (i < n - 1)     { dup2(pipe_fds[1], STDOUT_FILENO);
                                  close(pipe_fds[0]); close(pipe_fds[1]); }
            else               { dup2(client_fd, STDOUT_FILENO);
                                  dup2(client_fd, STDERR_FILENO); }

            char* args[16];
            char* sp;
            int ac = 0;
            for (char* a = strtok_r(cmds[i], " \t\n", &sp); a && ac < 15;
                 a = strtok_r(nullptr, " \t\n", &sp))
                args[ac++] = a;
            args[ac] = nullptr;
            if (args[0]) execvp(args[0], args);
            _exit(1);
        }

        if (pid > 0) {
            if (!pgid) pgid = pid;
            pids[npids++] = pid;
        }
        if (prev_fd != -1) close(prev_fd);
        if (i < n - 1) { close(pipe_fds[1]); prev_fd = pipe_fds[0]; }
        else prev_fd = -1;
    }

    for (int i = 0; i < npids; ++i)
        waitpid(pids[i], nullptr, 0);
}

static void handle_client(int client_fd, char* buf) {
    memset(buf, 0, BUFFER_SIZE);
    if (read(client_fd, buf, BUFFER_SIZE - 1) <= 0) { close(client_fd); return; }

    char* body = strstr(buf, "\r\n\r\n");
    if (body) {
        char* start = strstr(body, "\"command\": \"");
        if (start) {
            start += 12;
            char* end = strchr(start, '"');
            if (end) *end = '\0';
            const char* hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n";
            write(client_fd, hdr, strlen(hdr));
            execute_pipeline(start, client_fd);
        }
    }
    close(client_fd);
}


struct WorkQueue {
    std::queue<int>         q;
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done{false};

    void push(int fd) {
        { std::lock_guard lk(mu); q.push(fd); }
        cv.notify_one();
    }

    int pop() {
        std::unique_lock lk(mu);
        cv.wait(lk, [this] { return !q.empty() || done; });
        if (q.empty()) return -1;
        int fd = q.front(); q.pop();
        return fd;
    }

    void shutdown() {
        { std::lock_guard lk(mu); done = true; }
        cv.notify_all();
    }
};

static void worker(pp::PacketPool& pool, WorkQueue& queue) {
    while (true) {
        int fd = queue.pop();
        if (fd < 0) break;

        char* buf = static_cast<char*>(pool.allocate());
        handle_client(fd, buf);
        pool.deallocate(buf);
    }
}


int main() {
    signal(SIGPIPE, SIG_IGN);

    pp::PacketPool pool(BUFFER_SIZE, N_WORKERS);
    WorkQueue      queue;

    std::vector<std::thread> workers;
    workers.reserve(N_WORKERS);
    for (int i = 0; i < N_WORKERS; ++i)
        workers.emplace_back(worker, std::ref(pool), std::ref(queue));

    int srv = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(srv, BACKLOG);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = srv;
    epoll_ctl(epfd, EPOLL_CTL_ADD, srv, &ev);

    std::cout << "Thread-pool server: " << N_WORKERS << " workers, port " << PORT
              << ", PacketPool buffers=" << pool.block_count()
              << " × " << pool.block_size() << "B\n";

    std::array<epoll_event, MAX_EVENTS> events;
    while (true) {
        int n = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            while (true) {
                int client = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (client < 0) break;
                int flags = fcntl(client, F_GETFL);
                fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
                queue.push(client);
            }
        }
    }

    queue.shutdown();
    for (auto& t : workers) t.join();
    close(epfd);
    close(srv);
}
