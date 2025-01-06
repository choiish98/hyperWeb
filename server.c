#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>

#define PORT 8080

#define MAX_FLOW 10000
#define THREAD_POOL_SIZE 8
#define MAX_EVENTS (MAX_FLOW * THREAD_POOL_SIZE)

#define HTTP_HEADER_LEN 1024
#define FILE_SIZE 1024
#define BUFFER_SIZE 1024

typedef struct
{
	int keep_alive;
	int rsp_sent;

	char fcontent[FILE_SIZE];
	long int fsize;
} ctx_t;

typedef struct 
{
	int epoll_fd;
	ctx_t ctx[MAX_FLOW];
} task_t;

void set_nonblocking(int socket) 
{
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl get");
        exit(EXIT_FAILURE);
    }
    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl set");
        exit(EXIT_FAILURE);
    }
}

void clean_ctx(ctx_t *ctx)
{
	ctx->keep_alive = 0;
	ctx->rsp_sent = 0;
}

void close_connection(int epoll_fd, int socket_id)
{
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, socket_id, NULL);
	close(socket_id);
}

int accept_connection(task_t *task, int server_socket)
{
    int client_socket = accept(server_socket, NULL, NULL);
	if (client_socket >= 0) {
		if (client_socket >= MAX_FLOW) {
			printf("Invalid socket id %d\n", client_socket);
			return -1;
		}

		struct epoll_event event;
		event.events = EPOLLIN;
		event.data.fd = client_socket;
		set_nonblocking(client_socket);
		epoll_ctl(task->epoll_fd, EPOLL_CTL_ADD, client_socket, &event); 
	} else {
		if (errno != EAGAIN) {
			perror("Accept failed");
		}
	}

	return client_socket;
}

int handle_write(int client_socket, task_t *task, ctx_t *ctx)
{
	if (!ctx->rsp_sent) {
		return 0;
	}

	if (write(client_socket, ctx->fcontent, ctx->fsize) < 0) {
		perror("Error writing request");
	}

	if (ctx->keep_alive) {
		struct epoll_event event;
		event.events = EPOLLIN;
		event.data.fd = client_socket;
		epoll_ctl(task->epoll_fd, EPOLL_CTL_MOD, client_socket, &event);

		clean_ctx(ctx);
	} else {
		close_connection(task->epoll_fd, client_socket);
	}

	return ctx->fsize;
}

int handle_read(int client_socket, task_t *task, ctx_t *ctx)
{
	char buffer[BUFFER_SIZE];
    int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        return bytes_read;
    }

	/* http request handling */

    FILE *file = fopen("index.html", "r");
    if (!file) {
		perror("Error fopen");
        const char *error = "HTTP/1.1 404 Not Found\r\n\r\nFile not found.";
        if (write(client_socket, error, strlen(error)) < 0) {
			perror("Error writing request");
		}
        return bytes_read;
    }

    ctx->fsize = fread(ctx->fcontent, 1, FILE_SIZE, file);
    fclose(file);

    char response[HTTP_HEADER_LEN];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %zu\r\n"
             "Content-Type: text/html\r\n"
             "Connection: close\r\n"
             "\r\n",
             ctx->fsize);

    if (write(client_socket, response, strlen(response)) < 0) {
		perror("Error writing request");
	}

	ctx->rsp_sent = true;

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = client_socket;
	epoll_ctl(task->epoll_fd, EPOLL_CTL_MOD, client_socket, &event);

	handle_write(client_socket, task, ctx);

	return bytes_read;
}

task_t *init_server(void)
{
	task_t *task;

	task = (task_t *) calloc(1, sizeof(task_t));
	if (!task) {
		perror("calloc");
		return NULL;
	}

    task->epoll_fd = epoll_create1(0);
    if (task->epoll_fd == -1) {
        perror("Epoll creation failed");
        exit(EXIT_FAILURE);
    }

	return task;
}

int create_socket(int epoll_fd)
{
    int server_socket;
    struct sockaddr_in server_addr;
	struct epoll_event event;

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

	if (epoll_fd != 0) {
		int opt = 1;
		setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	}

    set_nonblocking(server_socket);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, 
				(struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
		return -1;
    }

    if (listen(server_socket, 5) < 0) {
        perror("Listen failed");
		return -1;
    }

    event.events = EPOLLIN;
    event.data.fd = server_socket;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event); 

	return server_socket;
}

void *server_thread(void *arg)
{
	int core = *(int *)arg;
	task_t *task;
    int server_socket, epoll_fd;
    struct epoll_event events[MAX_EVENTS];
	int do_accept;

    task = init_server();
	if (!task) {
		printf("failed to init_server()\n");
		exit(EXIT_FAILURE);
	}
	epoll_fd = task->epoll_fd;

	server_socket = create_socket(epoll_fd);
	if (server_socket < 0) {
		printf("Failed to create_socket()\n");
		exit(EXIT_FAILURE);
	}

    printf("Server is running on port %d...\n", PORT);

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
			if (errno != EINTR) {
	            perror("Epoll wait failed");
			}
			break;
        }

		do_accept = false;
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_socket) {
				do_accept = true;
            } else if (events[i].events & EPOLLERR) {
				printf("[CPU %d] Error on socket %d\n", 
						core, events[i].data.fd);
				close_connection(epoll_fd, events[i].data.fd);
			} else if (events[i].events & EPOLLIN) {
				int ret = handle_read(events[i].data.fd, task,
						&task->ctx[events[i].data.fd]);
				if (ret == 0) {
					close_connection(epoll_fd, events[i].data.fd);
				} else if (ret < 0) {
					if (errno != EAGAIN) {
						close_connection(epoll_fd, events[i].data.fd);
					}
					printf("[CPU %d]: Error occured at socket %d\n",
							core, events[i].data.fd);
				}
            } else if (events[i].events & EPOLLOUT) {
				ctx_t *ctx = &task->ctx[events[i].data.fd];
				if (ctx->rsp_sent) {
					handle_write(events[i].data.fd, task, ctx);
				} else {
					printf("Socket %d: Response header not sent yet\n",
							events[i].data.fd);
				}
			} else {
				assert(0);
			}
        }

		if (do_accept) {
			while (1) {
				int ret = accept_connection(task, server_socket);
				if (ret < 0)
					break;
			}
		}
    }

    close_connection(epoll_fd, server_socket);
	pthread_exit(NULL);

	return NULL;
}

int main(void) 
{
	int cores[THREAD_POOL_SIZE];

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        cores[i] = i;
        if (pthread_create(&threads[i], NULL, server_thread, &cores[i]) != 0) {
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		pthread_join(threads[i], NULL);
	}

    return 0;
}
