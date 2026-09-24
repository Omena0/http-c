#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <linux/limits.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <errno.h>

#define REQ_BUF_SIZE 8192

typedef struct {
    char type[8];
    char path[256];
    char http_ver[16];
} request_t;

bool resolve_path(const char *base, const char *path, char *out, size_t out_size) {
    char combined[PATH_MAX];

    snprintf(combined, sizeof(combined), "%s%s", base, path);

    char resolved[PATH_MAX];
    if (!realpath(combined, resolved)) return false;

    char real_base[PATH_MAX];
    if (!realpath(base, real_base)) return false;

    size_t len = strlen(real_base);

    if (strncmp(resolved, real_base, len) != 0) return false;
    if (resolved[len] != '/' && resolved[len] != '\0') return false;

    snprintf(out, out_size, "%s", resolved);
    return true;
}

bool parse_headers(const char *headers, request_t *req) {
    char line[512];

    const char *end = strstr(headers, "\r\n");
    if (!end) return false;

    size_t len = end - headers;
    if (len >= sizeof(line)) return false;

    memcpy(line, headers, len);
    line[len] = '\0';

    char *saveptr;
    char *method = strtok_r(line, " ", &saveptr);
    char *path   = strtok_r(NULL, " ", &saveptr);
    char *ver    = strtok_r(NULL, " ", &saveptr);

    if (!method || !path || !ver) return false;

    snprintf(req->type, sizeof(req->type), "%s", method);
    snprintf(req->path, sizeof(req->path), "%s", path);
    snprintf(req->http_ver, sizeof(req->http_ver), "%s", ver);

    if (strcmp(req->path, "/") == 0) {
        snprintf(req->path, sizeof(req->path), "/index.html");
    }

    return true;
}

const char* get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return NULL;

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";

    if (strcmp(ext, ".css") == 0)
        return "text/css";

    if (strcmp(ext, ".js") == 0)
        return "application/javascript";

    if (strcmp(ext, ".png") == 0)
        return "image/png";

    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";

    if (strcmp(ext, ".gif") == 0)
        return "image/gif";

    if (strcmp(ext, ".svg") == 0)
        return "image/svg+xml";

    return NULL; // Dont include any content type, the browser knows better.
}

void handle(int cs, const char *base_path) {
    char buf[REQ_BUF_SIZE] = {0};

    ssize_t total = 0;

    while (!strstr(buf, "\r\n\r\n") && total < REQ_BUF_SIZE - 1) {
        ssize_t len = recv(cs, buf + total, REQ_BUF_SIZE - 1 - total, 0);
        if (len <= 0) {
            close(cs);
            return;
        }
        total += len;
        buf[total] = '\0';
    }

    request_t req;

    if (!parse_headers(buf, &req)) {
        dprintf(cs,
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n"
        );
        close(cs);
        return;
    }

    if (strcmp(req.type, "GET") != 0) {
        dprintf(cs,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n"
        );
        close(cs);
        return;
    }

    char *q = strchr(req.path, '?');
    if (q) *q = '\0';

    char full_path[PATH_MAX];

    if (!resolve_path(base_path, req.path, full_path, sizeof(full_path))) {
        dprintf(cs,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n"
        );
        close(cs);
        return;
    }

    int fd = open(full_path, O_RDONLY);
    if (fd == -1) {
        dprintf(cs,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n"
        );
        close(cs);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        dprintf(cs,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n"
        );
        close(fd);
        close(cs);
        return;
    }

    const char *ctype = get_content_type(req.path);

    // If ctype is NULL we dont add the header.
    if (ctype) {
        dprintf(cs,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %jd\r\n"
            "Connection: close\r\n\r\n",
            ctype,
            (intmax_t)st.st_size
        );
    } else {
        dprintf(cs,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: %jd\r\n"
            "Connection: close\r\n\r\n",
            (intmax_t)st.st_size
        );
    }

    char file_buf[1024];

    ssize_t r;
    while ((r = read(fd, file_buf, sizeof(file_buf))) > 0) {
        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(cs, file_buf + written, r - written);
            if (w <= 0) {
                close(fd);
                close(cs);
                return;
            }
            written += w;
        }
    }

    close(fd);
    close(cs);
}

int sock;

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void sigint_handler(int sig) {
    (void)sig;
    close(sock);
    exit(0);
}

int main(int argc, char *argv[]) {
    const char *base_path = (argc > 1) ? argv[1] : ".";

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    if (listen(sock, 5) == -1) {
        perror("listen");
        return 1;
    }

    struct sigaction sa;

    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    while (1) {
        int cs = accept(sock, NULL, NULL);
        if (cs == -1) continue;

        pid_t pid = fork();
        if (pid == 0) {
            close(sock);
            handle(cs, base_path);
            exit(0);
        } else if (pid > 0) {
            close(cs);
        } else {
            perror("fork");
            close(cs);
        }
    }
}