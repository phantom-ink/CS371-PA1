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
#include <errno.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define EPOLL_TIMEOUT 1000  // Timeout in milliseconds
#define MICRO_TO_SECONDS 1000000LL

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int epoll_fd;              // Epoll instance FD
    int socket_fd;             // UDP socket FD
    long long total_rtt;       // Total RTT for acknowledged packets (us)
    long total_messages;       // Total number of successfully acknowledged packets
    float request_rate;        // Computed request rate (msgs/s)
    int client_id;             // Unique thread ID
    long tx_cnt;               // Transmitted packet count (first transmissions only)
    long rx_cnt;               // Received (echoed) packet count
} client_thread_data_t;

// Frame Header for PA2
typedef struct {
    int client_id;              // Identify Client Thread
    int seq_num;                // Sequence number for each packet
    char payload[MESSAGE_SIZE]; // Message payload
} packet_t;

/*
 * Function: client_thread_func:
 * Runs in a separate client thread to handle communication with the server
 * Uses UDP sockets to implement Stop-and-Wait + ARQ
 * Sends packets and waits for an echo from the server
 */
void *client_thread_func(void *arg) 
{
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    packet_t packet, recv_packet;
    struct timeval start, end;
    int seq_num = 0;

    // Initialize counts
    data->total_rtt = 0;
    data->total_messages = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;

    // Register UDP socket with epoll instance
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) 
    {   perror("epoll_ctl: socket_fd registration failed");
        pthread_exit(NULL);
    }

    // For each request, send a packet and wait until a valid echo is received
    for (int i = 0; i < num_requests; i++) 
    {   // Prepare packet header
        packet.client_id = data->client_id;
        packet.seq_num = seq_num;
        memset(packet.payload, 'A' + (seq_num % 26), MESSAGE_SIZE);

        // For each request, send packet once
        if (gettimeofday(&start, NULL) == -1) 
        {   perror("gettimeofday start error");
            break;
        }
        if (send(data->socket_fd, &packet, sizeof(packet), 0) != sizeof(packet)) {
            perror("send failed");
            break;
        }
        data->tx_cnt++;  // Only increment the first transmission
            
        int acked = 0;
        while (!acked) 
        {
            int nready = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT);
            if (nready == -1) 
            {   perror("epoll_wait failed");
                break;
            } else if (nready == 0) 
            {   // Timeout: log and retransmit without incrementing tx_cnt
                fprintf(stderr, "Timeout on client %d, seq %d. Retransmitting packet...\n",
                        data->client_id, seq_num);
                if (gettimeofday(&start, NULL) == -1) 
                {   perror("gettimeofday start error");
                    break;
                }
                // Retransmit the packet
                if (send(data->socket_fd, &packet, sizeof(packet), 0) != sizeof(packet)) 
                {   perror("send failed");
                    break;
                }
                continue;   // Important: Do NOT update tx_cnt again
            }
            // Process the events returned by epoll_wait
            for (int j = 0; j < nready; j++) {
                if (events[j].data.fd == data->socket_fd) 
                {   // Receive echoed packet
                    int recv_bytes = recv(data->socket_fd, &recv_packet, sizeof(recv_packet), 0);
                    if (recv_bytes != sizeof(recv_packet)) 
                    {   perror("recv failed");
                        continue;
                    }
                    // Check if the packet matches
                    if (recv_packet.client_id == data->client_id && recv_packet.seq_num == seq_num) 
                    {   // Retrieve ending timestamp
                        if (gettimeofday(&end, NULL) == -1) 
                        {   perror("gettimeofday end error");
                            break;
                        }
                        long long rtt = (end.tv_sec - start.tv_sec) * MICRO_TO_SECONDS +
                                        (end.tv_usec - start.tv_usec);
                        data->total_rtt += rtt;
                        data->total_messages++;
                        data->rx_cnt++;
                        acked = 1;   // Packet ACK'd
                        break;
                    } else 
                    {
                        fprintf(stderr, "Out-of-order or duplicate packet on client %d\n",
                                data->client_id);
                    }
                }
            }
        }
        seq_num++;  // Prepare next sequence number
    }

    // Compute per-thread request rate
    if (data->total_rtt > 0) { data->request_rate = (float)data->total_messages / (data->total_rtt / 1000000.0); } 
    else { data->request_rate = 0; }

    // Print metrics for this client thread and close
    printf("Client %d: TX=%ld, RX=%ld, Lost=%ld, Avg RTT=%lld us, Rate=%f msgs/s\n",
           data->client_id, data->tx_cnt, data->rx_cnt,
           data->tx_cnt - data->rx_cnt,
           (data->total_messages > 0) ? data->total_rtt / data->total_messages : 0,
           data->request_rate);

    pthread_exit(NULL);
}

/*
 * Function: run_client:
 * Create UDP socket and epoll instance for each client thread
 * Create client threads and aggregate metrics
 */
void run_client() 
{
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // Set up server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) 
    {   perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // For each client thread, create a UDP socket and epoll instance
    for (int i = 0; i < num_client_threads; i++) 
    {   // Create UDP socket
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) 
        {   perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        // Connect socket to the server
        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
        {   perror("UDP connect failed");
            exit(EXIT_FAILURE);
        }
        // Create an epoll instance for the client thread
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0) 
        {   perror("epoll_create1 failed");
            exit(EXIT_FAILURE);
        }
        // Assign unique client ID and initialize metrics
        thread_data[i].client_id = i;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
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
    long total_tx = 0, total_rx = 0;
    for (int i = 0; i < num_client_threads; i++) 
    {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
    }

    // Calculate and print aggregated metrics
    long long average_rtt = (total_messages > 0) ? total_rtt / total_messages : 0;
    printf("Overall: Average RTT: %lld us, Total Request Rate: %f msgs/s\n", average_rtt, total_request_rate);
    printf("Total Packets: TX=%ld, RX=%ld, Lost=%ld\n", total_tx, total_rx, total_tx - total_rx);

    // Close all the sockets and epoll file descriptors
    for (int i = 0; i < num_client_threads; i++) {
        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }
}

/*
 * Function: run_server:
 *  Creates a UDP socket, binds to the server address,
 *  and uses epoll in a run-to-completion loop to receive packets and echo them back
 */
void run_server() 
{
    int sock_fd, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    struct epoll_event event, events[MAX_EVENTS];
    socklen_t client_len = sizeof(client_addr);

    // Create UDP socket
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) 
    {   perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow address reuse
    int opt = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) 
    {   perror("setsockopt SO_REUSEADDR failed");
        exit(EXIT_FAILURE);
    }

    // Set up the server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) 
    {   perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // Bind the socket
    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
    {   perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Create an epoll instance and register the socket
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) 
    {   perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }
    event.events = EPOLLIN;
    event.data.fd = sock_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) < 0) 
    {   perror("epoll_ctl: sock_fd registration failed");
        exit(EXIT_FAILURE);
    }

    printf("UDP server listening on %s:%d\n", server_ip, server_port);

    /* Server's run-to-completion event loop */
    while (1) 
    {
        int nready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nready < 0) 
        {   perror("epoll_wait failed");
            break;
        }
        // Process each triggered event
        for (int i = 0; i < nready; i++) 
        {
            if (events[i].data.fd == sock_fd) 
            {
                packet_t packet;
                // Receive packet
                int recv_bytes = recvfrom(sock_fd, &packet, sizeof(packet), 0,
                                          (struct sockaddr *)&client_addr, &client_len);
                if (recv_bytes != sizeof(packet)) 
                {   perror("recvfrom failed");
                    continue;
                }
                // Echo the packet back to the client
                if (sendto(sock_fd, &packet, sizeof(packet), 0,
                           (struct sockaddr *)&client_addr, client_len) != sizeof(packet)) 
                {   perror("sendto failed");
                }
            }
        }
    }
    // Clean up socket and epoll instance
    close(sock_fd);
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