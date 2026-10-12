// The desktop simulator's web server: the /teams page and its data, the same page the Orb
// serves, so teams can be picked in a browser while testing without a device.
//
//     http://localhost:8080/teams        (SIM_WEB_PORT changes the port; 0 turns it off)
//
// Simulator only (see platformio.ini's native build_src_filter). Plain POSIX sockets on one
// background thread, answering one request per connection. The page's work is done by
// score_store's shared functions, exactly as on the device.
#include "sim_web.h"
#include "score_store.h"
#include "teams_page.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void reply(int fd, int code, const char *type, const std::string &body) {
    const char *why = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 302 ? "Found" : code == 400 ? "Bad Request" : "Not Found";
    char head[256];
    snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
             code, why, type, body.size(), code == 302 ? "Location: /teams\r\n" : "");
    std::string out = std::string(head) + body;
    size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t w = send(fd, out.data() + sent, out.size() - sent, 0);
        if (w <= 0) break;
        sent += (size_t)w;
    }
}

void handle(int fd) {
    std::string req;
    char buf[4096];
    size_t headEnd = std::string::npos, want = 0;
    while (true) {
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        req.append(buf, (size_t)r);
        if (headEnd == std::string::npos && (headEnd = req.find("\r\n\r\n")) != std::string::npos) {
            const size_t cl = req.find("Content-Length:");
            want = (cl != std::string::npos && cl < headEnd) ? (size_t)atoi(req.c_str() + cl + 15) : 0;
        }
        if (headEnd != std::string::npos && req.size() >= headEnd + 4 + want) break;
        if (req.size() > 64 * 1024) break;
    }
    if (headEnd == std::string::npos) return;
    char method[8] = "", path[256] = "";
    sscanf(req.c_str(), "%7s %255s", method, path);
    const std::string body = req.substr(headEnd + 4, want);
    std::string p = path, query;
    const size_t q = p.find('?');
    if (q != std::string::npos) { query = p.substr(q + 1); p = p.substr(0, q); }

    if (p == "/" ) { reply(fd, 302, "text/plain", "see /teams"); return; }
    if (p == "/teams") { reply(fd, 200, "text/html", TEAMS_PAGE); return; }
    if (p == "/teams.json") {
        char out[1024];
        score_store::teams_json(out, sizeof(out));
        reply(fd, 200, "application/json", out);
        return;
    }
    if (p == "/teamlist") {
        std::string lg;
        const size_t k = query.find("league=");
        if (k != std::string::npos) lg = query.substr(k + 7, query.find('&', k) - (k + 7));
        for (char &c : lg) c = (char)tolower((unsigned char)c);
        const char *json = nullptr;
        const int code = score_store::team_list_reply(lg.c_str(), &json);
        reply(fd, code, code == 200 ? "application/json" : "text/plain", code == 200 ? json : code == 202 ? "fetching" : "not offered");
        return;
    }
    if (p == "/teamsave" && !strcmp(method, "POST")) {
        char err[96];
        if (score_store::save_teams(body.c_str(), err, sizeof(err))) reply(fd, 200, "text/plain", "ok");
        else reply(fd, 400, "text/plain", err);
        return;
    }
    reply(fd, 404, "text/plain", "not here: the simulator only serves /teams");
}

void serve(int port) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);        // this computer only
    if (bind(s, (sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
        printf("[web] could not listen on port %d (SIM_WEB_PORT picks another)\n", port);
        close(s);
        return;
    }
    printf("[web] teams page: http://localhost:%d/teams\n", port);
    while (true) {
        const int fd = accept(s, nullptr, nullptr);
        if (fd < 0) continue;
        handle(fd);
        close(fd);
    }
}

}  // namespace

void sim_web_start() {
    const char *env = getenv("SIM_WEB_PORT");
    const int port = env ? atoi(env) : 8080;
    if (port <= 0) return;
    std::thread(serve, port).detach();
}
