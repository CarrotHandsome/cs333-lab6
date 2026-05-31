/*
    With snippets copied from lab assignment doc & src directory examples
*/

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
#define BUF_SIZE 8
//#define MAX_PLAYERS 5
#define REGISTER_CHAR '\r'
#define TIMER_NS 16666667 // ~1/60th sec

void sigint_handler(int sig);
int find_client(struct sockaddr_in cli, struct sockaddr_in registered[]);
void generate_terrain(moon_state_t * moon);
void interval_update(moon_state_t * moon, int sockfd, int timerfd, struct sockaddr_in cliaddr[]
    , socklen_t addrlens[]);
void player_input(player_t * player, int input);
void player_flying_update(player_t * player);
void player_reset(player_t * player);
int find_slot(moon_state_t * moon);

volatile sig_atomic_t keep_running = true;
static bool is_verbose = false;

int main(int argc, char * argv[]) {
    moon_state_t moon;

    int opt = 0;

    int port = DEFAULT_PORT;
    int socketfd = 0;
    int timerfd = 0;
    //int num_players = 0;
    struct sockaddr_in servaddr;
    struct sockaddr_in cliaddrs[MAX_PLAYERS];
    socklen_t addrlens[MAX_PLAYERS];
    struct sockaddr_in incoming_cli;    
    socklen_t incoming_addrlen = sizeof(struct sockaddr_in);
    ssize_t received_bytes = 0;
    //char recv_buffer[BUF_SIZE];    
    int recv_buffer = 0;
    int current_player = 0;

    struct pollfd pollfds[3];
    struct timespec now;
    struct itimerspec timer;

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

    /*
     * MAIN LOOP
     */
    while(keep_running) {

        poll(pollfds, 3, -1);

        if (pollfds[0].revents & POLLIN) {
            //handle input 
        }

        if (pollfds[1].revents & POLLIN) {
            received_bytes = recvfrom(socketfd, &recv_buffer, BUF_SIZE, 0
            , (struct sockaddr *) &incoming_cli, &incoming_addrlen);
        
            if (received_bytes < 1) {
                continue;
            }
            
            fprintf(stderr, "%d ", recv_buffer);
            
            fprintf(stderr, "\n");
            
            
            if (recv_buffer == REGISTER_CHAR) {
                current_player = find_slot(&moon);
                if (current_player == -1) {
                    if (is_verbose) {
                        fprintf(stderr, "No room. Registration failed.\n");
                    }
                    continue;
                }
                
                cliaddrs[current_player] = incoming_cli;
                addrlens[current_player] = incoming_addrlen;
                moon.players[current_player] = (player_t){
                    .active = true,
                    .id = current_player,
                    .x = 1 + (GRID_W / MAX_PLAYERS) * current_player,
                    .y = 0,
                    .velo_y = 0,
                    .fuel = INITIAL_FUEL_MED,
                    .thrust_level = 0,
                    .status = PREFLIGHT,
                };
                snprintf(moon.players[current_player].private_msg, PRIVATE_MSG_LEN, "Welcome Player %c", 'A' + current_player);

                //++num_players;

                if (is_verbose) {
                    fprintf(stderr, "New client registered\n");
                }
                continue;
            }

            if ((current_player = find_client(incoming_cli, cliaddrs)) == -1) {
                continue;
            }

            player_input(&(moon.players[current_player]), recv_buffer);              
        }

        if (pollfds[2].revents & POLLIN) {
            interval_update(&moon, socketfd, timerfd, cliaddrs, addrlens);
        }
    }

}

void player_flying_update(player_t * player) {
    
}

void interval_update(moon_state_t * moon, int sockfd, int timerfd, struct sockaddr_in cliaddrs[]
    , socklen_t addrlens[]) {
    uint64_t read_buffer;
    player_t * player = NULL;
    float actual_thrust = 0;
    bool winner = false;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player = &(moon->players[i]);
        if (!(player->active)) continue;
        moon->pilot = 'A' + (char)i;
        moon->color = i;

        switch (player->status) {
        case PREFLIGHT:
            break;
        case FLYING:
            actual_thrust = player->fuel > 0 ? (player->thrust_level / MAX_THRUST_LEVEL) * MAX_THRUST : 0;
            player->velo_y += (GRAVITY - actual_thrust);
            player->y += player->velo_y;

            if (player->velo_y >= VELO_Y_CRASH_THRESHOLD_MED) {
                snprintf(player->private_msg, PRIVATE_MSG_LEN, "1202: DANGEROUS RATE OF DESCENT");
            } else if (player->y < 0) {
                snprintf(player->private_msg, PRIVATE_MSG_LEN, "Caution: High Altitude");
            } else {
                snprintf(player->private_msg, PRIVATE_MSG_LEN, "Steady as she goes");
            }

            if (player->y >= moon->terrain[(int)player->x] - 1) {
                player->y = moon->terrain[(int)player->x] - 1;
                if (player->velo_y >= VELO_Y_CRASH_THRESHOLD_MED) {
                    player->status = CRASHED;
                    snprintf(player->private_msg, PRIVATE_MSG_LEN, "Player %c Crash Landed.", 'A' + i);
                    snprintf(moon->global_msg, GLOBAL_MSG_LEN, "KONAMI: Impact detected at grid %d (velocity %.2f).", (int)player->x, player->velo_y );
                } else {
                    for (int j = 0; j < MAX_PLAYERS; ++j) {
                        if (moon->players[j].active && moon->players[j].status == LANDED) {
                            winner = true;
                        }
                    }
                    player->status = LANDED;
                    if (!winner) {
                        snprintf(player->private_msg, PRIVATE_MSG_LEN, "Congratulations");
                        snprintf(moon->global_msg, GLOBAL_MSG_LEN, "Player %c is the winner! Final Velocity: %.2f", 'A' + i, player->velo_y);
                    } else {
                        snprintf(player->private_msg, PRIVATE_MSG_LEN, "Safe touchdown");
                    }
                }
                player->thrust_level = 0;
                player->velo_y = 0;
            }
            player->fuel = MAX(player->fuel - player->thrust_level * FUEL_BURN_RATE, 0);
            if (player->fuel == 0) {
                snprintf(player->private_msg, PRIVATE_MSG_LEN, "1202: FUEL X - BRACE FOR IMPACT");
            } else if (player->fuel <= FUEL_LOW_WARNING) {
                snprintf(player->private_msg, PRIVATE_MSG_LEN, "Caution: Low fuel");
            } 
            break;
        case CRASHED:
            
            break;
        case LANDED:
            break;
    }
        sendto(sockfd, moon, sizeof(moon_state_t), 0, (struct sockaddr *) &cliaddrs[i]
            , addrlens[i]);
    }

    read(timerfd, &read_buffer, sizeof(uint64_t));
}

//return 0 if player quit, 1 otherwise
void player_input(player_t * player, int input) {
    switch (input) {
        case 'w':
        case 'k':
        case KEY_UP:
            player->thrust_level = MIN(player->thrust_level + THROTTLE_INCREMENT_MED, MAX_THRUST_LEVEL);
            break;
        case 'a':
        case 'h':
        case KEY_LEFT:
            player->x = MAX(player->x - 1, 1);
            break;
        case 's':
        case 'j':
        case KEY_DOWN:
            player->thrust_level = MAX(player->thrust_level - THROTTLE_INCREMENT_MED, 0);
            break;
        case 'd':
        case 'l':
        case KEY_RIGHT:
            player->x = MIN(player->x + 1, GRID_W - 2);
            break;
        case ' ':
            player->thrust_level = 0;
            break;
        case 'L':
            player->status = FLYING;
            break;
        case 'R':
            player_reset(player);
            break;
        case 'Q':
            player->active = false;
            break;
        default:
            break;
    }
}

void player_reset(player_t * player) {
    player->x = 1 + (GRID_W / MAX_PLAYERS) * player->id;
    player->y = 0;
    player->fuel = INITIAL_FUEL_MED;
    player->private_msg[0] = '\0';
    player->thrust_level = 0;
    player->velo_y = 0;
    player->status = PREFLIGHT;
    snprintf(player->private_msg, PRIVATE_MSG_LEN, "Player %c Reset", 'A' + player->id);
}

void generate_terrain(moon_state_t * moon) {
    for (int i = 0; i < GRID_W; ++i) {
        moon->terrain[i] = 20;
    }
}

//return index of the first vacant slot
int find_slot(moon_state_t * moon) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!moon->players[i].active) return i;
    }
    return -1;
}

//return index of registered client or -1 if not registered
int find_client(struct sockaddr_in cli, struct sockaddr_in registered[]) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (registered[i].sin_addr.s_addr == cli.sin_addr.s_addr
            && registered[i].sin_port == cli.sin_port) {
            return i;
        }
    }

    if (is_verbose) {
        fprintf(stderr, "Unregistered client needs to send '\\r' to register first\n");
    }
    return -1;
}

void sigint_handler(int sig) {
    keep_running = false;
}