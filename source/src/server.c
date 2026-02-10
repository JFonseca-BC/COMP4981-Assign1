/*
 * server.c
 * Multi-threaded/Multiplexed HTTP Server (using poll)
 * Implements GET and HEAD methods.
 * Handles 200, 404, and 501 status codes.
 * Usage: ./server -p <port>
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* --- Named Constants --- */
enum ServerConstants {
  MAX_EVENTS = 1024,
  BUFFER_SIZE = 4096,
  TIME_STR_SIZE = 64,
  HTTP_OK = 200,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_NOT_IMPLEMENTED = 501,
  MAX_PORT = 65535,
  LISTEN_BACKLOG = 10,
  METHOD_BUF_SIZE = 16,
  PATH_BUF_SIZE = 256,
  PROTO_BUF_SIZE = 16,
  BASE_TEN = 10
};

static const char DEFAULT_DIR[] = ".";

/* --- Function Prototypes --- */
void log_message(const char *level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
int set_nonblocking(int fd);
void send_response(int client_fd, int status_code, const char *status_text,
                   const char *header_content_type, const char *body_data,
                   int is_head);
void serve_file(int client_fd, const char *path, int is_head);
void handle_client(int client_fd);

/* --- Logging Function --- */
void log_message(const char *level, const char *format, ...) {
  const time_t now = time(NULL);
  struct tm t_buf;
  const struct tm *const t = localtime_r(&now, &t_buf);
  va_list args;

  if (t != NULL) {
    char time_str[TIME_STR_SIZE];
    (void)strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    (void)fprintf(stderr, "[%s] [%s] ", time_str, level);
  }

  va_start(args, format);
  (void)vfprintf(stderr, format, args);
  va_end(args);
  (void)fprintf(stderr, "\n");
}

/* --- Helper: Set Socket Non-Blocking --- */
int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Helper: Send HTTP Response --- */
void send_response(int client_fd, int status_code, const char *status_text,
                   const char *header_content_type, const char *body_data,
                   int is_head) {
  char header[BUFFER_SIZE];
  const size_t content_length = (body_data != NULL) ? strlen(body_data) : 0;

  (void)snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status_code, status_text, header_content_type, content_length);

  (void)send(client_fd, header, strlen(header), 0);

  if (is_head == 0 && body_data != NULL && content_length > 0) {
    (void)send(client_fd, body_data, content_length, 0);
  }
}

/* --- Helper: Serve File --- */
void serve_file(int client_fd, const char *path, int is_head) {
  char full_path[BUFFER_SIZE];
  FILE *file = NULL;

  /* TRAVERSAL FIX: Block and send response immediately */
  if (strstr(path, "..") != NULL) {
    log_message("WARN", "Traversal attempt blocked: %s", path);
    send_response(client_fd, HTTP_FORBIDDEN, "Forbidden", "text/plain",
                  "403 Forbidden", is_head);
    return;
  }

  if (strcmp(path, "/") == 0) {
    (void)snprintf(full_path, sizeof(full_path), "%s/index.html", DEFAULT_DIR);
  } else {
    (void)snprintf(full_path, sizeof(full_path), "%s%s", DEFAULT_DIR, path);
  }

  file = fopen(full_path, "rbe");
  if (file == NULL) {
    log_message("ERROR", "File not found: %s", full_path);
    send_response(client_fd, HTTP_NOT_FOUND, "Not Found", "text/plain",
                  "404 Not Found", is_head);
    return;
  }

  {
    long fsize_raw = 0;
    size_t fsize = 0;
    (void)fseek(file, 0, SEEK_END);
    fsize_raw = ftell(file);
    fsize = (fsize_raw > 0) ? (size_t)fsize_raw : 0;
    (void)fseek(file, 0, SEEK_SET);

    {
      char *const file_content = malloc(fsize + 1);
      if (file_content != NULL) {
        const char *ctype = "text/plain";
        (void)fread(file_content, 1, fsize, file);
        file_content[fsize] = '\0';

        if (strstr(full_path, ".html") != NULL) {
          ctype = "text/html";
        }

        send_response(client_fd, HTTP_OK, "OK", ctype, file_content, is_head);
        log_message("INFO", "Served: %s (%zu bytes)", full_path, fsize);
        free(file_content);
      }
    }
  }
  (void)fclose(file);
}

/* --- Request Handler --- */
void handle_client(int client_fd) {
  char buffer[BUFFER_SIZE];
  ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

  if (bytes_read > 0) {
    char method[METHOD_BUF_SIZE];
    char path[PATH_BUF_SIZE];
    char protocol[PROTO_BUF_SIZE];
    int parsed = 0;

    buffer[bytes_read] = '\0';
    parsed = sscanf(buffer, "%15s %255s %15s", method, path, protocol);

    if (parsed >= 2) {
      if (strcmp(method, "GET") == 0) {
        serve_file(client_fd, path, 0);
      } else if (strcmp(method, "HEAD") == 0) {
        serve_file(client_fd, path, 1);
      } else {
        log_message("WARN", "Method not implemented: %s", method);
        send_response(client_fd, HTTP_NOT_IMPLEMENTED, "Not Implemented",
                      "text/plain", "501 Not Implemented", 0);
      }
    }
  }
  /* ENSURE CONNECTION CLOSES FOR ALL REQUESTS */
  (void)close(client_fd);
}

/* --- Main Event Loop --- */
int main(int argc, char *argv[]) {
  int opt;
  long port_in = 0;
  int server_fd = -1;
  int nfds = 1;
  struct pollfd fds[MAX_EVENTS];

  while ((opt = getopt(argc, argv, "p:")) != -1) {
    if (opt == 'p') {
      char *endptr;
      port_in = strtol(optarg, &endptr, BASE_TEN);
      if (*endptr != '\0') {
        port_in = 0;
      }
    }
  }

  if (port_in <= 0 || port_in > MAX_PORT) {
    (void)fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return EXIT_FAILURE;
  }

  {
    const uint16_t port = (uint16_t)port_in;
    struct sockaddr_in server_addr = {0};
    const int reuse = 1;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
      (void)close(server_fd);
      return EXIT_FAILURE;
    }

    (void)listen(server_fd, LISTEN_BACKLOG);
    log_message("INFO", "Server listening on port %d", port);
  }

  fds[0].fd = server_fd;
  fds[0].events = POLLIN;

  while (1) {
    const int poll_count = poll(fds, (nfds_t)nfds, -1);
    int i = 0;
    int current_nfds = 0;

    if (poll_count < 0) {
      break;
    }

    current_nfds = nfds;
    for (i = 0; i < current_nfds; i++) {
      if ((fds[i].revents & POLLIN) != 0) {
        if (fds[i].fd == server_fd) {
          const int c_fd = accept(server_fd, NULL, NULL);
          if (c_fd >= 0 && nfds < MAX_EVENTS) {
            fds[nfds].fd = c_fd;
            fds[nfds].events = POLLIN;
            nfds++;
          }
        } else {
          handle_client(fds[i].fd);
          fds[i] = fds[nfds - 1];
          nfds--;
          i--;
        }
      }
    }
  }
  (void)close(server_fd);
  return 0;
}
