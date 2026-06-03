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
#include <termios.h>
#include "eoraptor_ui.h"

#define OPTIONS "P:vh"
#define BUF_SIZE 8
#define REGISTER_CHAR '\r'
#define TIMER_NS 16666667 // ~1/60th sec
#define MOON_HEIGHT 19

void sigint_handler(int sig);
int find_client(struct sockaddr_in cli, struct sockaddr_in registered[]);
void generate_terrain(moon_state_t * moon);
void interval_update(moon_state_t * moon, int sockfd, int timerfd, struct sockaddr_in cliaddr[]
    , socklen_t addrlens[]);
void player_input(player_t * player, int input);
void player_flying_update(player_t * player);
void player_reset(player_t * player);
int find_slot(moon_state_t * moon);
void render_moon(void);
int python_index(int i, int s);
long timespec_diff_ns(struct timespec a, struct timespec b);
int num_players(moon_state_t moon);

/* Animation variables */
char moon_chunk[8] = "*=+#@%&";
char twinkle_frames[4] = "+X";
int twinkle_framecount = 2;
int moon_framecount = 7;
int fps = 12;
int current_start_index = 0; //index of moon_chunk the first line of the moon rendering starts at
int moon_line_lead[MOON_HEIGHT] = {11, 8,  6,  4,  3,  2,  1,  1,  1,  1,  1,  1,  1,  2,  3,  4,  6,  8, 11};
int moon_line_len[MOON_HEIGHT] = {17, 23, 27, 31, 33, 35, 37, 37, 37, 37, 37, 37, 37, 35, 33, 31, 27, 23, 17};
char leadline[12] = "           ";
struct timespec moon_anim_start = {0};
struct timespec moon_anim_end = {0};
unsigned long elapsed = 0;
unsigned long ns_per_frame = 0;
int moon_x_offset = 10;
int moon_y_offset = 2;
char server_message[256] = {0};



volatile sig_atomic_t keep_running = true;
static bool is_verbose = false;

int main(int argc, char * argv[]) {

    moon_state_t moon;

    int opt = 0;

    int port = DEFAULT_PORT;
    int socketfd = 0;
    int timerfd = 0;
    struct sockaddr_in servaddr;
    struct sockaddr_in cliaddrs[MAX_PLAYERS];
    socklen_t addrlens[MAX_PLAYERS];
    struct sockaddr_in incoming_cli;    
    socklen_t incoming_addrlen = sizeof(struct sockaddr_in);
    ssize_t received_bytes = 0;
    int console_input = ' ';    
    char last_console_input = ' ';
    int recv_buffer = 0;
    int current_player = 0;    
    char num_to_char[6] = "01234";
    struct pollfd pollfds[3];
    struct timespec now;
    struct itimerspec timer;

    struct termios termio;
    struct termios termio_original;
    

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
                    fprintf(stdout, "Options for Eoraptor server:\n-P [PORT #] (use the same port # with the client)\n-v (verbose output)\n");
                    exit(EXIT_SUCCESS);
                    break;
            }            
        }
    }

    /* Set up ncurses and animation timer*/
    clock_gettime(CLOCK_MONOTONIC, &moon_anim_start);
    ns_per_frame = 1000000000 / fps;
    //initscr();
    //cbreak();
    //curs_set(0);

    /* initialize socket and timerfd*/
    if ((socketfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("socket creation error");
        return(EXIT_FAILURE);
    }

    //set up timer
    clock_gettime(CLOCK_MONOTONIC, &now);
    timer.it_value.tv_sec = now.tv_sec;
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

    pollfds[0].fd = STDIN_FILENO;
    pollfds[0].events = POLLIN;
    pollfds[1].fd = socketfd;
    pollfds[1].events = POLLIN;
    pollfds[2].fd = timerfd;
    pollfds[2].events = POLLIN;

    srandom(time(NULL) ^ getpid());
    generate_terrain(&moon);
    
    tcgetattr(STDIN_FILENO, &termio_original); //save current terminal input settings    
    tcgetattr(STDIN_FILENO, &termio);
    //termios equivalent to cbreak and noecho
    termio.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &termio);

    printf("\033[2J"); //clear screen
    printf("\033[?25l"); //hide cursor
    /*
     * MAIN LOOP
     */
    while(keep_running) {
        //Animation timing
        clock_gettime(CLOCK_MONOTONIC, &moon_anim_end);
        elapsed += timespec_diff_ns(moon_anim_end, moon_anim_start);
        if (elapsed >= ns_per_frame) {
            current_start_index = (current_start_index + 1) % moon_framecount;
            elapsed = 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &moon_anim_start);

        poll(pollfds, 3, -1);

        if (pollfds[0].revents & POLLIN) {
            last_console_input = console_input;            
            scanf("%c", (char *)&console_input);
            //fprintf(stderr, "scanned: %d\n", (int)console_input);       
            if (console_input == 10) {
                switch (last_console_input) {
                    case 'q':
                    case 'Q':
                        keep_running = false;
                        break;
                    case 'h':
                        strcpy(server_message, "q:quit, h:help, L:launch all, R:reset all, k:client controls, l:list clients");
                        break;
                    case 'L':
                        strcpy(server_message, "Launching All Players - Stand By");
                        for (int i = 0; i < MAX_PLAYERS; ++i) {
                            if (moon.players[i].active) {
                                moon.players[i].status = FLYING;
                            }
                        }
                        break;
                    case 'R':
                        strcpy(server_message, "Resetting All Players");
                        for (int i = 0; i < MAX_PLAYERS; ++i) {
                            if (moon.players[i].active) {
                                player_reset(&moon.players[i]);
                            }
                        }
                        break;
                    case 'k':
                        strcpy(server_message, "Client controls: Movement/thrust: wasd, hjkl, arrow keys; L: Launch; R: Reset; Q: Quit");
                        break;
                    case 'l':
                        memset(server_message, 0, sizeof(server_message));
                        for (int i = 0; i < MAX_PLAYERS; ++i) {
                            if (moon.players[i].active) {
                                server_message[strlen(server_message)] = 'A' + i;
                                strcat(server_message, ": Addr ");
                                inet_ntop(AF_INET, &(cliaddrs[i].sin_addr), &server_message[strlen(server_message)], INET_ADDRSTRLEN);
                                strcat(server_message, " Port ");
                                sprintf(&server_message[strlen(server_message)], "%u", ntohs(cliaddrs[i].sin_port));
                                // strcat(server_message, itoa((int)cliaddrs[i].sin_port));
                                strcat(server_message, "\n");
                            }
                        }

                        if (server_message[0] == '\0') {
                            strcpy(server_message, "No players yet");
                        }
                        break;
                    default:
                        strcpy(server_message, "Not a valid command");
                }
                console_input = ' ';
            } else if (console_input == KEY_BACKSPACE || console_input < 65 || console_input > 121) {
                console_input = ' ';
            }
            
        }
        if (!keep_running) break;

        if (pollfds[1].revents & POLLIN) {
            received_bytes = recvfrom(socketfd, &recv_buffer, BUF_SIZE, 0
            , (struct sockaddr *) &incoming_cli, &incoming_addrlen);
        
            if (received_bytes < 1) {
                continue;
            }  

            if (recv_buffer == REGISTER_CHAR) {
                current_player = find_slot(&moon);
                if (current_player == -1) {
                    if (is_verbose) {
                        strcpy(server_message, "No room. Registration failed.");                        
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
                    strcpy(server_message, "New client registered( )");
                    server_message[strlen(server_message) - 2] = num_to_char[num_players(moon)];
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
            //erase();
            //
            render_moon();
            //move(MOON_HEIGHT + 2, 0);
            printf("\033[H\033[2B\tTO\t\t   THE\t\t\tMOON");
            printf("\033[%d;0Hserver> %c", MOON_HEIGHT + 5, console_input);
            //mvaddstr(MOON_HEIGHT + 2, 0, "server >");
            //addch(console_input);
            printf("\033[%d;0H\033[2K%s", MOON_HEIGHT + 6, server_message);
            //mvaddstr(MOON_HEIGHT + 5, 0, server_message);
            //refresh();
        }
    }
    //endwin();
    printf("\033[H\033[2J\033[3J");
    printf("*****\nThank you for hosting EORAPTOR\n*****\n");
    tcsetattr(STDIN_FILENO, TCSANOW, &termio_original); //restore original terminal settings
    printf("\033[?25h"); //restore cursor

    close(socketfd);
    close(timerfd);
    close(pollfds[0].fd);
    close(pollfds[1].fd);
    close(pollfds[2].fd);
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
                        snprintf(server_message, 256, "Player %c crashed", 'A' + i);
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
                            snprintf(server_message, 256, "Player %c won", 'A' + i);
                            
                        } else {
                            snprintf(player->private_msg, PRIVATE_MSG_LEN, "Safe touchdown");
                            snprintf(server_message, 256, "Player %c landed safely", 'A' + i);
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
            --player->x;
            if (player->x < 1) {
                player->x = GRID_W - 2;
            }
            break;
        case 's':
        case 'j':
        case KEY_DOWN:
            player->thrust_level = MAX(player->thrust_level - THROTTLE_INCREMENT_MED, 0);
            break;
        case 'd':
        case 'l':
        case KEY_RIGHT:
            ++player->x;
            if (player->x > GRID_W - 2) {
                player->x = 1;
            }
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
    int x = 0;
    int current_height = GRID_H - 4;
    while (x < GRID_W) {
        // Pick how wide this feature will be
        int feature_width = (random() % 7) + 4;
        // Decide if this feature is a flat landing pad
        int is_flat = (random() % 10) < 6;
        for (int i = 0; i < feature_width && x < GRID_W; i++) {
            if (!is_flat) {
                // Mountains: slope up 1, down 1, or stay flat
                current_height += (random() % 3) - 1;
                // Keep the terrain from growing off the screen
                if (current_height < 5) {
                    current_height = 5;
                }
                if (current_height > (GRID_H - 1)) {
                    current_height = (GRID_H - 1);
                }
            }
            // If is_flat is true, current_height doesn't change!
            moon->terrain[x] = current_height;
            x++;
        }
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
        mvaddstr(25, 0, "Unregistered client needs to send '\\r' to register first");
    }
    return -1;
}

void sigint_handler(int sig) {
    keep_running = false;
}

void render_moon(void) {
    int line_start_index = 0;
    char line_buf[64];
    char moon_buff[1024];
    int linelen = 0;
    int total_lead = 0;
    int buf_cursor = 0;
    //move(moon_y_offset, moon_x_offset);
    //addstr(leadline);
    //strcpy(moon_buff, leadline);
    //memset(&moon_buff, 32, sizeof(moon_buff)); //prefill buffer with empty spaces
    total_lead = moon_line_lead[0] + moon_x_offset;
    linelen = total_lead + moon_line_len[0];
    for (int i = 0; i < total_lead; ++i) {
        line_buf[i] = ' ';
    }
    for (int i = 0; i < moon_line_len[0]; ++i) {
        line_buf[i + total_lead] = moon_chunk[(current_start_index + i) % moon_framecount];
    }
    line_buf[linelen] = '\n';    
    strcpy(moon_buff, line_buf);
    buf_cursor = linelen + 1;
    //moon_buff[linelen] = '\n';
    //addnstr(line_buf, moon_line_len[0]);
    for (int i = 1; i < MOON_HEIGHT; ++i) {
        total_lead = moon_line_lead[i] + moon_x_offset;
        linelen = total_lead + moon_line_len[i];
        //move(moon_y_offset + i, moon_x_offset);        
        //addnstr(leadline, moon_line_lead[i]);
        line_start_index = python_index(
            current_start_index + 7 - (moon_line_lead[0] - moon_line_lead[i]) - i,
            moon_framecount
        );
        for (int j = 0; j < total_lead; ++j) {
            line_buf[j] = ' ';
        }
        for (int j = 0; j < moon_line_len[i]; ++j) {
            line_buf[j + total_lead] = moon_chunk[(line_start_index + j) % moon_framecount];
        }
        line_buf[linelen] = '\n';
        strcpy(&moon_buff[buf_cursor], line_buf);
        buf_cursor += linelen + 1;
        //addnstr(line_buf, moon_line_len[i]);        
    }
    moon_buff[buf_cursor] = '\0';
    printf("\033[H\033[3B%s", moon_buff);
}

int num_players(moon_state_t moon) {
    int p = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (moon.players[i].active) ++p;
    }
    return p;
}

//wraps negative indices to go backward from end of array
int python_index(int i, int s) {
    return ((i % s) + s) % s;
}

long timespec_diff_ns(struct timespec a, struct timespec b) {
    return(a.tv_sec * 1000000000 - b.tv_sec * 1000000000) + (a.tv_nsec - b.tv_nsec);
}