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

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_EVENTS 30000
#define THREAD_POOL_SIZE 8

typedef struct {
    int client_socket;
} task_t;

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	task_t queue[MAX_EVENTS];
	int queue_start, queue_end, queue_size;
} thread_queue_t;

thread_queue_t thread_queues[THREAD_POOL_SIZE];

void set_nonblocking(int socket) {
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

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_read;

    bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // No data available, try again later
        } else {
			printf("socket %d: bytes_read = %d\n", client_socket, bytes_read);
            perror("Error reading request");
            close(client_socket);
            return;
        }
    } else if (bytes_read == 0) {
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0'; 
    FILE *file = fopen("index.html", "r");
    if (!file) {
        const char *error = "HTTP/1.1 404 Not Found\r\n\r\nFile not found.";
        if (write(client_socket, error, strlen(error)) < 0) {
			perror("Error writing request");
		}
        close(client_socket);
        return;
    }

    char file_content[BUFFER_SIZE];
    size_t file_size = fread(file_content, 1, BUFFER_SIZE, file);
    fclose(file);

    char response[BUFFER_SIZE + 128];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %zu\r\n"
             "Content-Type: text/html\r\n"
             "Connection: close\r\n"
             "\r\n%s",
             file_size, file_content);

    if (write(client_socket, response, strlen(response)) < 0) {
		perror("Error writing request");
	}
    close(client_socket);
}

void enqueue_task(int thread_id, task_t task) {
	thread_queue_t *queue = &thread_queues[thread_id];

    pthread_mutex_lock(&queue->lock);

    if (queue->queue_size == MAX_EVENTS) {
        printf("Thread %d queue is full, dropping connection\n", thread_id);
        close(task.client_socket);
    } else {
        queue->queue[queue->queue_end] = task;
        queue->queue_end = (queue->queue_end + 1) % MAX_EVENTS;
        queue->queue_size++;
        pthread_cond_signal(&queue->cond);
    }

    pthread_mutex_unlock(&queue->lock);
}

void *worker_thread(void *arg) {
	int thread_id = *(int *)arg;
	thread_queue_t *queue = &thread_queues[thread_id];

    while (true) {
        pthread_mutex_lock(&queue->lock);

        while (queue->queue_size == 0) {
            pthread_cond_wait(&queue->cond, &queue->lock);
        }

        task_t task = queue->queue[queue->queue_start];
        queue->queue_start = (queue->queue_start + 1) % MAX_EVENTS;
        queue->queue_size--;

        pthread_mutex_unlock(&queue->lock);

        handle_client(task.client_socket);
    }
    return NULL;
}

int main() {
    int server_socket, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event event, events[MAX_EVENTS];
	int cores[THREAD_POOL_SIZE];

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_mutex_init(&thread_queues[i].lock, NULL);
        pthread_cond_init(&thread_queues[i].cond, NULL);
        thread_queues[i].queue_start = 0;
        thread_queues[i].queue_end = 0;
        thread_queues[i].queue_size = 0;
    }

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        cores[i] = i;
        if (pthread_create(&threads[i], NULL, worker_thread, &cores[i]) != 0) {
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_socket);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) < 0) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d...\n", PORT);

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll creation failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    event.events = EPOLLIN;
    event.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1) {
        perror("Epoll add failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("Epoll wait failed");
            close(server_socket);
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_socket) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
                if (client_socket == -1) {
                    perror("Accept failed");
                    continue;
                }

                set_nonblocking(client_socket);

                event.events = EPOLLIN | EPOLLET;
                event.data.fd = client_socket;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event) == -1) {
                    perror("Epoll add client failed");
                    close(client_socket);
                }
            } else {
				int thread_id = events[i].data.fd % THREAD_POOL_SIZE;
                task_t task = { .client_socket = events[i].data.fd };
                enqueue_task(thread_id, task);
            }
        }
    }

    close(server_socket);
    return 0;
}
