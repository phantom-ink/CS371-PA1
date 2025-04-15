/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here
# Student #1: Mathew Damrell
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define EPOLL_TIMEOUT 5000
#define MICRO_TO_SECONDS 1000000LL

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg)
{   /* TODO:
     * It sends messages to the server, waits for a response using epoll,
     * and measures the round-trip time (RTT) of this request-response.
     */

    /* TODO:
     * The function exits after sending and receiving a predefined number of messages (num_requests). 
     * It calculates the request rate based on total messages and RTT
     */

    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* Send 16-Bytes message every time */
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    // Initialize counts
    data->total_rtt = 0;
    data->total_messages = 0;

    // Register connected client socket with epoll instance
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1)
    {   perror("epoll_ctl: socket_fd registration failed");
        pthread_exit(NULL);
    }

    // Loop for num_requests requests
    for (int i = 0; i < num_requests; i++)
    {   // Retrieve starting timestamp
        if (gettimeofday(&start, NULL) == -1)
        {   perror("gettimeofday start error");
            break;
        }
        // Send the message to the server
        int sent_bytes = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
        if (sent_bytes != MESSAGE_SIZE)
        {   perror("send failed");
            break;
        }
        // Wait for the server's response
        // Request times out after 5 seconds (5000ms) to not block indefinitely
        int nready = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT);
        if (nready == -1)
        {   perror("epoll_wait failed");
            break;
        } else if (nready == 0)
        {   fprintf(stderr, "Timeout waiting for response\n");
            break;
        }
        // Process the events returned by epoll_wait
        for (int j = 0; j < nready; j++)
        {   // Check if socket is ready
            if (events[j].data.fd == data->socket_fd)
            {
                int recv_bytes = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                if (recv_bytes != MESSAGE_SIZE)
                {   perror("recv failed");
                    break;
                }
                // Retrieve ending timestamp
                if (gettimeofday(&end, NULL) == -1)
                {   perror("gettimeofday end error");
                    break;
                }
                // Calculate RTT in microseconds (us)
                long long rtt = (end.tv_sec - start.tv_sec) * MICRO_TO_SECONDS +
                                (end.tv_usec - start.tv_usec);
                data->total_rtt += rtt;
                data->total_messages += 1;
            }
        }
    }
    // Calculate the request rate in messages/second
    if (data->total_rtt > 0) data->request_rate = (float)data->total_messages / (data->total_rtt / (double)MICRO_TO_SECONDS);
    else data->request_rate = 0;

    pthread_exit(NULL);
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client()
{   /* TODO:
    * Create sockets and epoll instances for client threads
    * and connect these sockets of client threads to the server
    */

    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // Set up server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // Convert server IP address from text to binary
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {   perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // For each client thread, create a socket and epoll instance, then connect to server
    for (int i = 0; i < num_client_threads; i++)
    {   // Create TCP socket
        thread_data[i].socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (thread_data[i].socket_fd < 0) 
        {   perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        // Connect socket to the server
        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
        {   perror("connect failed");
            exit(EXIT_FAILURE);
        }
        // Create an epoll instance for the client thread
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0)
        {   perror("epoll_create1 failed");
            exit(EXIT_FAILURE);
        }
        // Initialize per-thread RTT and message counts
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
    }
    
    // Hint: use thread_data to save the created socket and epoll instance for each thread
    // You will pass the thread_data to pthread_create() as below
    for (int i = 0; i < num_client_threads; i++)
    {
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0)
        {   perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }

    // Wait for all client threads to complete
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
    }

    // Calculate and print aggregated metrics
    long long average_rtt = (total_messages > 0) ? total_rtt / total_messages : 0;
    printf("Average RTT: %lld us\n", average_rtt);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);

    // Close all the sockets and epoll file descriptors
    for (int i = 0; i < num_client_threads; i++)
    {   close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }
}

void run_server()
{   /* TODO:
    * Server creates listening socket and epoll instance.
    * Server registers the listening socket to epoll
    */

    int listen_fd, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event event, events[MAX_EVENTS];

    // Create listening TCP socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {   perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set up the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // Convert server IP address from text to binary
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {   perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // Bind listening socket to specified IP and port
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {   perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Start listening for incoming connections
    if (listen(listen_fd, SOMAXCONN) < 0)
    {   perror("listen failed");
        exit(EXIT_FAILURE);
    }

    // Create an epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {   perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    // Register listening socket with epoll instance
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0)
    {   perror("epoll_ctl: listen_fd registration failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s:%d\n", server_ip, server_port);

    /* Server's run-to-completion event loop */
    while (1) 
    {   /* TODO:
         * Server uses epoll to handle connection establishment with clients
         * or receive the message from clients and echo the message back
         */
        
        int nready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nready < 0)
        {   perror("epoll_wait failed");
            break;
        }

        // Process each triggered event
        for (int i = 0; i < nready; i++)
        {
            int event_fd = events[i].data.fd;

            // If event is on the listening socket -> new incoming connection
            if (event_fd == listen_fd)
            {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0)
                {   perror("accept failed");
                    continue;
                }
                // Register new client socket with epoll for read events
                event.events = EPOLLIN;
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
                {   perror("epoll_ctl: client_fd registration failed");
                    close(client_fd);
                    continue;
                }
                printf("Accepted new connection: fd %d\n", client_fd);
            } else
            {   // If event on client socket -> read incoming message
                char buffer[MESSAGE_SIZE];
                int bytes_received = recv(event_fd, buffer, MESSAGE_SIZE, 0);
                if (bytes_received <= 0)
                {
                    if (bytes_received == 0) printf("Client fd %d closed the connection\n", event_fd); // The client closed the connection
                    else perror("recv failed");
                    // Remove client socket from epoll monitoring and close it
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, event_fd, NULL);
                    close(event_fd);
                } else
                {   // Echo the received message back to the client
                    int bytes_sent = send(event_fd, buffer, bytes_received, 0);
                    if (bytes_sent != bytes_received) perror("send failed");
                }
            }
        }
    }
    // Clean up listening socket and epoll instance
    close(listen_fd);
    close(epoll_fd);
}


/*////////////////
// DO NOT EDIT //
/*//////////////
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}