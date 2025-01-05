#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_EVENTS 10

void set_nonblocking(int s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl get");
        exit(EXIT_FAILURE);
    }

    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl set");
        exit(EXIT_FAILURE);
    }
}

void handle_client(int c) {
    char buffer[BUFFER_SIZE];
    int bytes_read;

    // Read the reque	st (not fully parsing for simplicity)
    bytes_read = read(c, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available, continue processing later
            return;
        } else {
            perror("Error reading request");
            close(c);
            return;
        }
    } else if (bytes_read == 0) {
        // Client disconnected
        printf("Client disconnected\n");
        close(c);
        return;
    }

    buffer[bytes_read] = '\0'; // Null-terminate the request
    printf("Received request:\n%s\n", buffer);

    // Open the index.html file
    FILE *file = fopen("index.html", "r");
    if (!file) {
        perror("Error opening file");
        const char *error_response = "HTTP/1.1 404 Not Found\r\n\r\nFile not found.";
        if (write(c, error_response, strlen(error_response)) < 0) {
			perror("Error write failed");
		}
        close(c);
        return;
    }

    // Read the file content
    char file_content[BUFFER_SIZE];
    size_t file_size = fread(file_content, 1, BUFFER_SIZE, file);
    fclose(file);

    // Create HTTP response
    char response[BUFFER_SIZE + 128];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %zu\r\n"
             "Content-Type: text/html\r\n"
             "Connection: close\r\n"
             "\r\n%s",
             file_size, file_content);

    // Send the response
    if (write(c, response, strlen(response)) < 0) {
		perror("Error write failed");
	}

    // Close the socket
    close(c);
}

int main(void) {
    int s;
	int epoll_fd;
    struct sockaddr_in saddr;
    struct epoll_event event, events[MAX_EVENTS];

    // Create a socket
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

	// Set the socket to non-blocking
    set_nonblocking(s);

    // Set up the server address structure
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(PORT);

    // Bind the socket to the address and port
    if (bind(s, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
        perror("Bind failed");
        close(s);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(s, 5) < 0) {
        perror("Listen failed");
        close(s);
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d...\n", PORT);

    // Create an epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll creation failed");
        close(s);
        exit(EXIT_FAILURE);
    }

    // Add the server socket to the epoll instance
    event.events = EPOLLIN;
    event.data.fd = s;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s, &event) == -1) {
        perror("Epoll add failed");
        close(s);
        exit(EXIT_FAILURE);
    }

    // Main loop to accept and handle clients
    while (1) {
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("Epoll wait failed");
            close(s);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == s) {
                // Accept new connections
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int c = accept(s, (struct sockaddr *)&caddr, &clen);
                if (c == -1) {
                    perror("Accept failed");
                    continue;
                }

                set_nonblocking(c);

                event.events = EPOLLIN | EPOLLET;
                event.data.fd = c;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c, &event) == -1) {
                    perror("Epoll add client failed");
                    close(c);
                    continue;
                }
            } else {
                // Handle client requests
                handle_client(events[i].data.fd);
            }
        }
    }

    // Close the server socket
    close(s);
    return 0;
}
