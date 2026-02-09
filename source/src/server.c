/*
 * server.c
 * Multi-threaded/Multiplexed HTTP Server (using poll)
 * Implements GET and HEAD methods.
 * Handles 200, 404, and 501 status codes.
 * Usage: ./server -p <port>
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/stat.h>
#include <stdarg.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define DEFAULT_DIR "."

/* --- Logging Function --- */
/* Formats logs with timestamps and levels to stderr */
void log_message(const char *level, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[64];
    
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    
    fprintf(stderr, "[%s] [%s] ", time_str, level);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
}

/* --- Helper: Set Socket Non-Blocking --- */
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Helper: Send HTTP Response --- */
void send_response(int client_fd, int status_code, const char *status_text, const char *content_type, const char *body, int is_head) {
    char header[BUFFER_SIZE];
    long content_length = body ? strlen(body) : 0;
    
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "\r\n",
             status_code, status_text, content_type, content_length);

    // Send Header
    send(client_fd, header, strlen(header), 0);

    // Send Body (only if not HEAD and body exists)
    if (!is_head && body && content_length > 0) {
        send(client_fd, body, content_length, 0);
    }
}

/* --- Helper: Serve File --- */
void serve_file(int client_fd, const char *path, int is_head) {
    char full_path[BUFFER_SIZE];
    
    // Prevent directory traversal attacks
    if (strstr(path, "..")) {
        send_response(client_fd, 403, "Forbidden", "text/plain", "403 Forbidden", is_head);
        return;
    }

    // Default to index.html if root requested
    if (strcmp(path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s/index.html", DEFAULT_DIR);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s", DEFAULT_DIR, path);
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        log_message("ERROR", "File not found: %s", full_path); 
        send_response(client_fd, 404, "Not Found", "text/plain", "404 Not Found", is_head);
        return;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *file_content = malloc(fsize + 1);
    if (file_content) {
        fread(file_content, 1, fsize, file);
        file_content[fsize] = 0;
        
        // Determine content type
        const char *ctype = "text/plain";
        if (strstr(full_path, ".html")) ctype = "text/html";
        else if (strstr(full_path, ".css")) ctype = "text/css";
        else if (strstr(full_path, ".js")) ctype = "application/javascript";

        send_response(client_fd, 200, "OK", ctype, file_content, is_head);
        log_message("INFO", "Served file: %s (%ld bytes)", full_path, fsize); 
        free(file_content);
    } else {
        send_response(client_fd, 500, "Internal Server Error", "text/plain", "500 Error", is_head);
    }
    fclose(file);
}

/* --- Request Handler --- */
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        
        char method[16], path[256], protocol[16];
        int parsed = sscanf(buffer, "%15s %255s %15s", method, path, protocol);

        if (parsed >= 2) {
            log_message("INFO", "Request: %s %s", method, path);

            // Handle Valid GET/HEAD Requests
            if (strcmp(method, "GET") == 0) {
                serve_file(client_fd, path, 0);
            } else if (strcmp(method, "HEAD") == 0) {
                serve_file(client_fd, path, 1);
            } else {
                // Handle Unsupported Method
                log_message("WARN", "Unsupported method: %s", method);
                send_response(client_fd, 501, "Not Implemented", "text/plain", "501 Not Implemented", 0);
            }
        } else {
            send_response(client_fd, 400, "Bad Request", "text/plain", "400 Bad Request", 0);
        }
    } else if (bytes_read == 0) {
        // Client closed connection
    } else {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            log_message("ERROR", "recv failed: %s", strerror(errno)); 
        }
    }
    close(client_fd); // Close after response (Simple HTTP style)
}

/* --- Main Server Setup & Loop --- */
int main(int argc, char *argv[]) {
    // 1. START / USAGE State: Parse Arguments with getopt
    int opt;
    int port = 0;

    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
        if (port != 0) {
            log_message("ERROR", "Invalid port number: %d", port);
        } else {
            log_message("ERROR", "Port number required via -p flag");
        }
        return EXIT_FAILURE;
    }

    // 2. INIT State: Create, Bind, Listen
    int server_fd;
    struct sockaddr_in server_addr;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        log_message("FATAL", "Socket creation failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int socket_opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt));
    set_nonblocking(server_fd);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_message("FATAL", "Bind failed: %s", strerror(errno)); 
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        log_message("FATAL", "Listen failed: %s", strerror(errno));
        close(server_fd);
        return EXIT_FAILURE;
    }

    log_message("INFO", "Server listening on port %d...", port); 

    // 3. EVENT_LOOP State (using poll)
    struct pollfd fds[MAX_EVENTS];
    int nfds = 1;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;

    while (1) {
        int poll_count = poll(fds, nfds, -1); // Wait indefinitely for events

        if (poll_count < 0) {
            log_message("ERROR", "Poll error: %s", strerror(errno));
            break;
        }

        int current_nfds = nfds;
        for (int i = 0; i < current_nfds; i++) {
            if (fds[i].revents == 0) continue;

            if (fds[i].revents & POLLIN) {
                if (fds[i].fd == server_fd) {
                    // 4. ACCEPT_CLIENT State
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

                    if (client_fd >= 0) {
                        log_message("INFO", "New connection from %s", inet_ntoa(client_addr.sin_addr));
                        if (nfds < MAX_EVENTS) {
                            fds[nfds].fd = client_fd;
                            fds[nfds].events = POLLIN;
                            nfds++;
                        } else {
                            log_message("WARN", "Max clients reached, closing connection.");
                            close(client_fd);
                        }
                    } else {
                        if (errno != EWOULDBLOCK && errno != EAGAIN) {
                            log_message("ERROR", "Accept failed");
                        }
                    }
                } else {
                    // 5. HANDLE_IO State
                    handle_client(fds[i].fd);
                    
                    // Remove from poll set (swap with last element)
                    fds[i] = fds[nfds - 1];
                    nfds--;
                    i--; // Recheck the new socket at this index
                }
            }
        }
    }

    // 6. CLEANUP State
    close(server_fd);
    return EXIT_SUCCESS;
}