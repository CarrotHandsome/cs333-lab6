#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <libgen.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include "eoraptor_ui.h"

#define OPTIONS "P:vh"
#define BUF_SIZE 1024
#define MAX_PLAYERS 4
#define REGISTER_CHAR '\r'
#define TIMER_NS 16666667 // ~1/60th sec

void sigint_handler(int sig);
int find_client(struct sockaddr_in cli, struct sockaddr_in registered[], int numcli);
void generate_terrain(moon_state_t * moon);
void interval_update(moon_state_t * moon, int sockfd, int timerfd, struct sockaddr_in cliaddr[]
    , socklen_t addrlens[], int numplayers);
void player_update(moon_state_t * moon, int player, char input);

volatile sig_atomic_t keep_running = true;
static bool is_verbose = false;

int main(int argc, char * argv[]) {
    moon_state_t moon;

    int opt = 0;

    int port = DEFAULT_PORT;
    int socketfd = 0;
    int timerfd = 0;
    int num_players = 0;
    struct sockaddr_in servaddr;
    struct sockaddr_in cliaddrs[MAX_PLAYERS];
    socklen_t addrlens[MAX_PLAYERS];// = sizeof(struct sockaddr_in);
    //player_t players[MAX_PLAYERS];
    struct sockaddr_in incoming_cli;    
    socklen_t incoming_addrlen = sizeof(struct sockaddr_in);
    ssize_t received_bytes = 0;
    char recv_buffer[BUF_SIZE];    
    int current_player = 0;
    struct pollfd pollfds[3];
    struct timespec now;
    struct itimerspec timer;
    //timer_t timerid;
    

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);    

    {
        while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
            switch (opt) {
                case 'P':      
                    sscanf(optarg, "%d", &port);              
                    break;
                case 'v':
                    is_verbose = true;
                    break;
                case 'h':
                    fprintf(stdout, "Helpful hints\n");
                    exit(EXIT_SUCCESS);
                    break;
            }            
        }
    }

    /* initialize socket and timerfd*/
    if ((socketfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("socket creation error");
        return(EXIT_FAILURE);
    }

    //set up timer
    clock_gettime(CLOCK_MONOTONIC, &now);
    timer.it_value.tv_sec = now.tv_nsec;
    timer.it_value.tv_nsec = now.tv_nsec + TIMER_NS;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_nsec = TIMER_NS;
    timerfd = timerfd_create(CLOCK_REALTIME, 0);
    timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &timer, NULL);

    memset(&servaddr, 0, sizeof(struct sockaddr_in));
    memset(&moon, 0, sizeof(moon_state_t));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socketfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) == -1) {
        perror("bind failure");
        return(EXIT_FAILURE);
    }

    if (is_verbose) {
        printf("Listening for connections on port %d\n", port);
    }

    //pollfds = calloc(3, sizeof(struct pollfd));
    pollfds[0].fd = STDIN_FILENO;
    pollfds[0].events = POLLIN;
    pollfds[1].fd = socketfd;
    pollfds[1].events = POLLIN;
    pollfds[2].fd = timerfd;
    pollfds[2].events = POLLIN;

    generate_terrain(&moon);
    while(keep_running) {

        poll(pollfds, 3, -1);

        if (pollfds[0].revents & POLLIN) {
            //handle input 
        }

        if (pollfds[1].revents & POLLIN) {
            received_bytes = recvfrom(socketfd, recv_buffer, BUF_SIZE, 0
            , (struct sockaddr *) &incoming_cli, &incoming_addrlen);
        
            if (received_bytes < 1) {
                continue;
            }

            if (recv_buffer[0] == REGISTER_CHAR) {
                if (num_players >= MAX_PLAYERS) {
                    if (is_verbose) {
                        fprintf(stderr, "Too many clients. Registration failed.\n");
                    }
                    continue;
                }
                cliaddrs[num_players] = incoming_cli;
                addrlens[num_players] = incoming_addrlen;
                moon.players[num_players] = (player_t){
                    .active = true,
                    .id = num_players,
                    .x = (GRID_W / MAX_PLAYERS) * num_players,
                    .y = 0,
                    .velo_y = 0,
                    .fuel = INITIAL_FUEL_MED,
                    .thrust_level = 0,
                    .status = PREFLIGHT,
                    .private_msg = "private msg"
                };

                ++num_players;

                if (is_verbose) {
                    fprintf(stderr, "New client registered\n");
                }
                continue;
            }

            if ((current_player = find_client(incoming_cli, cliaddrs, num_players)) == -1) {
                continue;
            }

            player_update(&moon, current_player, recv_buffer[0]);
        }

        if (pollfds[2].revents & POLLIN) {
            interval_update(&moon, socketfd, timerfd, cliaddrs, addrlens, num_players);
        }
    }

}

void interval_update(moon_state_t * moon, int sockfd, int timerfd, struct sockaddr_in cliaddrs[]
    , socklen_t addrlens[], int numplayers) {
    uint64_t read_buffer;
    //printf("numplayers:  %d\n", numplayers);

    for (int i = 0; i < numplayers; ++i) {
        moon->pilot = i;
        sendto(sockfd, moon, sizeof(moon_state_t), 0, (struct sockaddr *) &cliaddrs[i]
            , addrlens[i]);
    }

    read(timerfd, &read_buffer, sizeof(uint64_t));
}

void player_update(moon_state_t * moon, int player, char input) {
    switch (input) {
        case 'w':
        case 'k':
        case KEY_UP:
            break;
        case 'a':
        case 'h':
        case KEY_LEFT:
            break;
        case 's':
        case 'j':
        case KEY_DOWN:
            break;
        case 'd':
        case 'l':
        case KEY_RIGHT:
            break;
    }
}

void generate_terrain(moon_state_t * moon) {
    for (int i = 0; i < GRID_W; ++i) {
        moon->terrain[i] = 20;
    }
}

//return index of registered client or -1 if not registered
int find_client(struct sockaddr_in cli, struct sockaddr_in registered[], int numcli) {
    for (int i = 0; i < numcli; ++i) {
        if (registered[i].sin_addr.s_addr == cli.sin_addr.s_addr
            && registered[i].sin_port == cli.sin_port) {
            return i;
        }
    }

    if (is_verbose) {
        fprintf(stderr, "Unregistered client needs to send '\r' to register first\n");
    }
    return -1;
}

void sigint_handler(int sig) {
    keep_running = false;
}