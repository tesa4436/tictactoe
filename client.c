#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <stdlib.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include "constants.h"

static struct termios term, term_orig;
static char break_loop = 0;

typedef struct usr {
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
} User;

struct game_board {
	char *matrix;
	size_t board_size;
	char character, state; // the character written to the matrix
	unsigned long local_last_x, local_last_y, remote_last_x, remote_last_y;
};

struct session_details {
	User *logged_in_user;
	size_t bytes_written; // number of bytes written after the last operation
	struct game_board *current_game;
	char session_present, wait;
};

static unsigned char login_request(char*, struct session_details**);
static unsigned char logout_request(char*, struct session_details**);
static unsigned char create_user_request(char*, struct session_details**);
static unsigned char join_random_game_request(char*, struct session_details**);
static unsigned char create_new_game_request(char*, struct session_details**);
static unsigned char leave_game_request(char*, struct session_details**);
static unsigned char action_request(char*, struct session_details**);
static unsigned char action_notify(char*, struct session_details**);
static unsigned char other_player_present_notify(char*, struct session_details**);
static unsigned char leave_game_reply(char*, struct session_details**);
static unsigned char alloc_local_board(char*, struct session_details**);
static unsigned char action_reply(char*, struct session_details**);
static unsigned char game_is_finished(char*, struct session_details**);

static unsigned char (*handler[NUMBER_OF_OPCODES]) (char*, struct session_details**) = {
	[LOGIN_REQUEST] = login_request,	
	[LOGOUT_REQUEST] = logout_request,
	[CREATE_USER_REQUEST] = create_user_request,
	[JOIN_RANDOM_GAME_REQUEST] = join_random_game_request,
	[CREATE_NEW_GAME_REQUEST] = create_new_game_request,
	[LEAVE_GAME_REQUEST] = leave_game_request,
	[ACTION_REQUEST] = action_request,
	[ACTION_REPLY] = action_reply,
	[ACTION_NOTIFY] = action_notify,
	[LEAVE_GAME_REPLY] = leave_game_reply,
	[JOIN_RANDOM_GAME_REPLY] = alloc_local_board,
	[GAME_IS_FINISHED] = game_is_finished,
	[CREATE_NEW_GAME_SUCCESS] = alloc_local_board,
	[OTHER_PLAYER_PRESENT_NOTIFY] = other_player_present_notify
};

void print_board(struct game_board *board)
{
	if(!board) {
		return;
	}
	for(size_t i = 0; i < board->board_size; i++) {
		for(size_t j = 0; j < board->board_size; j++) {
			if(board->matrix[(board->board_size * i) + j]) {
				putc(board->matrix[(board->board_size * i) + j], stdout);
			} else {
				putc('_', stdout);
			}
			putc(' ', stdout);
		}
		putc('\n', stdout);
	}
}

void print_reply_code_meaning(const unsigned char ret_code)
{
	switch(ret_code) {
		case PEER_LEFT_NOTIFY:			printf("The other player disconnected.\n");						break;
		case CANNOT_WRITE_HERE:			printf("You tried to write to a board cell that is already written to.\n");		break;
		case ACTION_NOTIFY:			printf("The other player has made a move.\n");						break;
		case OTHER_PLAYER_PRESENT_NOTIFY:	printf("The other player has joined the game.\n");					break;
		case NO_FURTHER_ACTIONS_PERMITTED:	printf("The game is finished. No moves can be made.\n");				break;
		case NOT_YOUR_TURN:			printf("It's not your turn.\n");							break;
		case GAME_IS_FINISHED:			printf("The game is finished.\n");							break;
		case LEAVE_GAME_REPLY:			printf("You left the game.\n");								break;
		case JOIN_RANDOM_GAME_REPLY:		printf("You joined a random game.\n");							break;
		case ACTION_REPLY:			printf("The action has been successfully made on the board.\n");			break;
		case CREATE_NEW_GAME_SUCCESS:		printf("A new game has been created.\n");						break;
		case CREATE_USER_SUCCESS:		printf("A new user has been created.\n");						break;
		case LOGIN_SUCCESS:			printf("You successfully logged in.\n");						break;
		case LOGOUT_REPLY:			printf("You successfully logged out.\n");						break;
		case NO_PLAYER_PRESENT:			printf("The other player is not present in the game.\n");				break;
		case INVALID_OPERANDS:			printf("Your input is invalid, probably out of the board's bounds.\n");			break;
		case NO_GAMES_AVAILABLE:		printf("The server either hosts no games or no games are awaiting another player..\n");	break;
		case NOT_IMPLEMENTED:			printf("This feature is not implemented.\n");						break;
		case USER_ALREADY_EXISTS:		printf("A user with this name already exists.\n");					break;
		case INTERNAL_SERVER_ERROR:		printf("The server encountered a fatal internal error.\n");				break;
		case INVALID_REQUEST:			printf("The server deems this request as invalid.\n");					break;
		case IO_ERROR:				printf("Input/output error\n");								break;
		case LOGIN_FAILED:			printf("Login failed due to incorrect given credentials.\n");				break;
	}
}

void print_options()
{
	
}

int get_coordinates_from_buffer(char *buffer, unsigned long *x, unsigned long *y)
{
	if(!x || !y) {
		return 1;
	}
	char *next = NULL;
	/*
	for(unsigned long i = 0; i < BUFFER_LENGTH; ++i) {
		if(buffer[i])
			putc(buffer[i], stdout);
		else
			putc('.', stdout);
	}
	putc('\n', stdout);
	*/
	if(buffer[0] == '0' && buffer[1] == '\0') {
		*x = 0;
		next = buffer + 1;
	} else {
		*x = strtoul(buffer, &next, 10);
		if(!*x || errno == ERANGE) {
			return 2;
		}
	}
	if(!next && buffer[2] == '0' && buffer[3] == '\0') {
		*y = 0;
	} else if(next && next[1] == '0' && next[2] == '\0') {
		*y = 0;
	} else {
		*y = strtoul(++next, NULL, 10);
		if(!*y || errno == ERANGE) {
			return 3;
		}
	}
	return 0;
}

unsigned char other_player_present_notify(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	if((*session_details)->current_game->character == 'X' || (*session_details)->current_game->character == 'O') {
		(*session_details)->wait = 0;
		(*session_details)->current_game->character += 0x20;
	} else {
		(*session_details)->wait = 1;
	}
	return OTHER_PLAYER_PRESENT_NOTIFY;
}

unsigned char action_notify(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	unsigned long x, y;
	unsigned char ret_code;
	char *next = NULL;
	size_t size = (*session_details)->current_game->board_size; // no null check yet
	if(get_coordinates_from_buffer(buffer + 2, &x, &y)) {
		return INTERNAL_CLIENT_ERROR;
	}
	/*
	for(unsigned int i = 0; i < BUFFER_LENGTH; ++i) {
		if(buffer[i])
			putc(buffer[i], stdout);
		else	putc('.', stdout);
	}
	putc('\n', stdout);
	*/
	if(x > (*session_details)->current_game->board_size - 1 || y > (*session_details)->current_game->board_size - 1) {
		return INTERNAL_CLIENT_ERROR;
	}
	(*session_details)->current_game->matrix[(size * x) + y] = (*session_details)->current_game->character == 'x' ? 'o' : 'x';
	(*session_details)->wait = 0;
	(*session_details)->current_game->remote_last_x = x;
	(*session_details)->current_game->remote_last_y = y;
	if(buffer[1] == 'X' || buffer[1] == 'O') {
		(*session_details)->current_game->state = buffer[1];
		ret_code = GAME_IS_FINISHED;
	} else {
		ret_code = ACTION_NOTIFY;
	}
	return ret_code;
}

unsigned char action_reply(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	if(!(*session_details)->current_game) {
		return INVALID_REQUEST;
	}
	if((*session_details)->current_game->character == 'X' || (*session_details)->current_game->character == 'O') {
		(*session_details)->current_game->character += 0x20;
	}
	unsigned long x = (*session_details)->current_game->local_last_x;
	unsigned long y = (*session_details)->current_game->local_last_y;
	(*session_details)->current_game->matrix[((*session_details)->current_game->board_size * x) + y] = (*session_details)->current_game->character;
	return ACTION_REPLY;
}

unsigned char game_is_finished(char *buffer, struct session_details **session_details)
{
	if((*session_details)->current_game->character == buffer[1] + 0x20) {
		printf("You won the game.\n");
	} else if(buffer[1] == 'D') {
		printf("Tie.\n");
	} else {
		printf("You lost the game.\n");
	}
	free((*session_details)->current_game->matrix);
	free((*session_details)->current_game);
	(*session_details)->current_game = NULL;
	break_loop = 1;
	print_board((*session_details)->current_game);
	return GAME_IS_FINISHED;
}

unsigned char leave_game_reply(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	free((*session_details)->current_game->matrix);
	free((*session_details)->current_game);
	(*session_details)->current_game = NULL;
	return LEAVE_GAME_REPLY;
}

unsigned char alloc_local_board(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	(*session_details)->current_game = calloc(1, sizeof(struct game_board));
	if(!(*session_details)->current_game) {
		return INTERNAL_CLIENT_ERROR;
	}
	size_t board_size;
	(*session_details)->current_game->character = buffer[1];
	if(buffer[1] != 'x' && buffer[1] != 'o' && buffer[1] != 'X' && buffer[1] != 'O') {
		return INTERNAL_CLIENT_ERROR; // invalid data received
	}
	board_size = strtoul(buffer + 2, NULL, 10);
	if(!board_size || errno == ERANGE) {
		return INTERNAL_CLIENT_ERROR;
	}
	(*session_details)->current_game->board_size = board_size;
	(*session_details)->current_game->matrix = calloc(board_size, board_size);
	if(!(*session_details)->current_game->matrix) {
		free((*session_details)->current_game);
		return INTERNAL_CLIENT_ERROR;
	}
	return CREATE_NEW_GAME_SUCCESS;
}

unsigned char join_random_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	buffer[0] = JOIN_RANDOM_GAME_REQUEST;
	(*session_details)->bytes_written = 1;
	return JOIN_RANDOM_GAME_REQUEST;
}

unsigned char create_new_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	buffer[0] = CREATE_NEW_GAME_REQUEST;
	(*session_details)->bytes_written = 1;
	return CREATE_NEW_GAME_REQUEST;
}

unsigned char leave_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	buffer[0] = LEAVE_GAME_REQUEST;
	(*session_details)->bytes_written = 1;
	free((*session_details)->current_game->matrix);
	free((*session_details)->current_game);
	(*session_details)->current_game = NULL;
	return LEAVE_GAME_REQUEST;
}

unsigned char action_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	unsigned long x, y;
	char num_buffer[BUFFER_LENGTH];
	int n, n2;
	memset(num_buffer, 0, BUFFER_LENGTH);
	printf("enter coordinate x: ");
	fgets(num_buffer, BUFFER_LENGTH, stdin);
	if(num_buffer[0] == '0' && num_buffer[1] == '\n') { // this probably unnecessary?
		x = 0;
	} else {
		x = strtoul(num_buffer, NULL, 10);
		if(!x || errno == ERANGE) {
			return INVALID_REQUEST;
		}
	}
	printf("enter coordinate y: ");
	memset(num_buffer, 0, BUFFER_LENGTH);
	fgets(num_buffer, BUFFER_LENGTH, stdin);
	if(num_buffer[0] == '0' && num_buffer[1] == '\n') {
		y = 0;
	} else {
		y = strtoul(num_buffer, NULL, 10);
		if(!y || errno == ERANGE) {
			return INVALID_REQUEST;
		}
	}
	buffer[0] = ACTION_REQUEST;
	n = snprintf(buffer + 1, BUFFER_LENGTH / 2, "%lu", x);
	n2 = snprintf(buffer + 1 + n + 1, BUFFER_LENGTH / 2, "%lu", y);
	if(n2 <= 0 || n <= 0) {
		return INTERNAL_CLIENT_ERROR;
	}
	(*session_details)->bytes_written = 1 + n + n2 + 2;		
	/*
	for(size_t i = 0; i < (*session_details)->bytes_written; ++i) {
		if(buffer[i])
			putc(buffer[i], stdout);
		else	putc('.', stdout);
	}
	putc('\n', stdout);
	*/
	(*session_details)->current_game->local_last_x = x;
	(*session_details)->current_game->local_last_y = y;
	return ACTION_REQUEST;
}

unsigned char login_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		(*session_details)->bytes_written = 0;
		return INVALID_REQUEST;
	}
	size_t usernamelen, passwordlen;
	char input_buffer[BUFFER_LENGTH];
	char *c;
	memset(input_buffer, 0, sizeof(input_buffer));
	printf("username: ");
	fgets(input_buffer, sizeof(input_buffer), stdin);
	c = strchr(input_buffer, '\n');
	if(c) {
		*c = '\0';
	}
	strncpy((*session_details)->logged_in_user->username, input_buffer, USERNAMELEN);
	(*session_details)->logged_in_user->username[USERNAMELEN] = '\0';
	printf("password: ");
	tcsetattr(STDIN_FILENO, TCSANOW, &term);
	memset(input_buffer, 0, sizeof(input_buffer));
	fgets(input_buffer, sizeof(input_buffer), stdin);
	c = strchr(input_buffer, '\n');
	if(c) {
		*c = '\0';
	}
	strncpy((*session_details)->logged_in_user->password, input_buffer, PASSWORDLEN);
	(*session_details)->logged_in_user->password[PASSWORDLEN] = '\0';
	tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
	buffer[0] = LOGIN_REQUEST;
	usernamelen = strlen((*session_details)->logged_in_user->username);
	passwordlen = strlen((*session_details)->logged_in_user->password);
	if((usernamelen && passwordlen) && (usernamelen <= USERNAMELEN || passwordlen <= PASSWORDLEN)) {
		strncpy(buffer + 1,
			(*session_details)->logged_in_user->username,
			usernamelen);
		strncpy(buffer + usernamelen + 2,
			(*session_details)->logged_in_user->password,
			passwordlen + 1);
	}
	(*session_details)->bytes_written = 1 + usernamelen + passwordlen + 2; // opcode byte and two null terminators
	(*session_details)->session_present = 1;
	return LOGIN_REQUEST;
}

unsigned char logout_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		return INVALID_REQUEST;
	}
	buffer[0] = LOGOUT_REQUEST;
	(*session_details)->session_present = 0;
	(*session_details)->bytes_written = 1;
	return LOGOUT_REQUEST;
}

unsigned char create_user_request(char *buffer, struct session_details **session_details)
{
	if(*session_details) {
		(*session_details)->bytes_written = 0;
		return INVALID_REQUEST;
	}
	size_t usernamelen, passwordlen;
	printf("username: ");
	fgets((*session_details)->logged_in_user->username, USERNAMELEN + 2, stdin);
	printf("password: ");
	tcsetattr(STDIN_FILENO, TCSANOW, &term);
	fgets((*session_details)->logged_in_user->password, PASSWORDLEN + 2, stdin);
	tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
	buffer[0] = CREATE_USER_REQUEST;
	usernamelen = strlen((*session_details)->logged_in_user->username);
	passwordlen = strlen((*session_details)->logged_in_user->password);
	if((usernamelen && passwordlen) && (usernamelen <= USERNAMELEN || passwordlen <= PASSWORDLEN)) {
		strncpy(buffer + 1,
			(*session_details)->logged_in_user->username,
			usernamelen);
		strncpy(buffer + usernamelen + 2,
			(*session_details)->logged_in_user->password,
			passwordlen + 1);
	}
	(*session_details)->bytes_written = 1 + usernamelen + passwordlen + 2; // opcode byte and two null terminators
	return CREATE_USER_REQUEST;
}

void error(const char *msg)
{
	perror(msg);
	exit(3);
}
int main(int argc, char *argv[])
{
	int sockfd, portno, n;
	struct sockaddr_in serv_addr;
	struct hostent *server;
	char command[BUFFER_LENGTH];
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
	char buffer[BUFFER_LENGTH];
	unsigned char opcode, ret_code;
	struct session_details *session_details = NULL;
	tcgetattr(STDIN_FILENO, &term);
	term_orig = term;
	term.c_lflag &= ~ECHO;
	if (argc < 3) {
		fprintf(stderr,"usage %s hostname port\n", argv[0]);
		exit(0);
	}
	portno = atoi(argv[2]);
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) 
		error("ERROR opening socket");
	server = gethostbyname(argv[1]);
	if (server == NULL) {
		fprintf(stderr,"ERROR, no such host\n");
		exit(2);
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
	serv_addr.sin_port = htons(portno);
	if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
		error("ERROR connecting");
	session_details = calloc(1, sizeof(struct session_details));
	if(!session_details) {
		error("error on calloc");
	}
	session_details->logged_in_user = calloc(1, sizeof(User));
	if(!session_details->logged_in_user) {
		free(session_details);
		session_details = NULL;
		error("error on calloc");
	}
	session_details->current_game = NULL;
	while (!break_loop) {
		memset(buffer, 0 , BUFFER_LENGTH);
		memset(command, 0, BUFFER_LENGTH);
		memset(username, 0, USERNAMELEN + 1);
		memset(command, 0, PASSWORDLEN + 1);
		/*
		printf("enter an operation number: ");
		fgets(command, BUFFER_LENGTH, stdin);
		opcode = atoi(command);
		*/
		printf("enter 0 to login or 1 to create a user: ");
		fgets(command, BUFFER_LENGTH, stdin);
		if(command[0] == '0') {
			opcode = LOGIN_REQUEST;
		} else if(command[0] == '1') {
			opcode = CREATE_USER_REQUEST;
		} else {
			continue;
		}
		ret_code = handler[opcode](buffer, &session_details);
		if(ret_code == INVALID_REQUEST) {
			printf("\nerror on handler\n");
			continue;
		}
		n = send(sockfd, buffer, session_details->bytes_written, MSG_NOSIGNAL);
		if(n < 0) {
			error("ERROR writing to socket");
		}
		memset(buffer, 0, BUFFER_LENGTH);
		n = recv(sockfd, buffer, 255, 0);
		if(n < 0) {
			error("ERROR reading from socket");
		}
		if(!n) {
			error("error on recv");
		}
		printf("server sent code %u, %d bytes read\n", (unsigned char)*buffer, n);
		print_reply_code_meaning((unsigned char)*buffer);
		if((unsigned char)*buffer != LOGIN_SUCCESS && (unsigned char)*buffer != CREATE_USER_SUCCESS) {
			printf("some error occured\n");
			continue;
		} else {
			session_details->session_present = 1;
		}
		while(session_details->session_present) {
			memset(buffer, 0 , BUFFER_LENGTH);
			memset(command, 0, BUFFER_LENGTH);
			if(!session_details->current_game) {
				printf("enter an operation number (1 - log out,\n3 - join a random game,\n4 - create a new game): ");
				fgets(command, BUFFER_LENGTH, stdin);
				opcode = atoi(command);
				if(handler[opcode]) {
					ret_code = handler[opcode](buffer, &session_details);
					if(ret_code == INTERNAL_CLIENT_ERROR) {
						fprintf(stderr, "error occured, exiting...\n");
						exit(ret_code);	
					} else if(ret_code == INVALID_REQUEST) {
						fprintf(stderr, "invalid request\n");
						continue;
					}
				} else {
					printf("not implemented\n");
					continue;
				}
			} else {
				printf("enter an operation number (5 - leave the game,\n6 - make a move): ");
				fgets(command, BUFFER_LENGTH, stdin);
				opcode = atoi(command);
				if(opcode == 5 || opcode == 6) {
					ret_code = handler[opcode](buffer, &session_details);
				} else {
					continue;
				}
				if(ret_code == INTERNAL_CLIENT_ERROR) {
					fprintf(stderr, "error occured, exiting...\n");
					exit(ret_code);
				} else if(ret_code == INVALID_REQUEST) {
						fprintf(stderr, "invalid request\n");
						continue;
				}
			}
			n = send(sockfd, buffer, session_details->bytes_written, MSG_NOSIGNAL);
			if(n < 0) {
				error("ERROR writing to socket");
			}
			memset(buffer, 0 , BUFFER_LENGTH);
			n = recv(sockfd, buffer, 255, 0);
			if(n <= 0) {
				error("ERROR reading from socket");
			}
			printf("SERVER sent code %u, %d bytes read\n", (unsigned char)*buffer, n);
			print_reply_code_meaning((unsigned char)*buffer);
			if(handler[(unsigned char)*buffer]) {
				ret_code = handler[(unsigned char)*buffer](buffer, &session_details);
				if(ret_code == INTERNAL_CLIENT_ERROR) {
					fprintf(stderr, "error occured, exiting...\n");
					exit(ret_code);	
				} else if(ret_code == INVALID_REQUEST) {
						fprintf(stderr, "invalid request\n");
						continue;
				}

			}
			switch((unsigned char)*buffer) {
				case INTERNAL_SERVER_ERROR: {
					print_reply_code_meaning((unsigned char)*buffer);
					free(session_details->current_game->matrix);
					free(session_details->current_game);
					session_details->current_game = NULL;
					session_details->session_present = 0;
					break_loop = 1;

				} break;
				case JOIN_RANDOM_GAME_REPLY: {
					if(buffer[1] == 'x' || buffer[1] == 'o')
						case CREATE_NEW_GAME_SUCCESS: // skip above check for these codes, looks a bit ugly i admit
						case ACTION_REPLY: {
						for(;;) {
							memset(buffer, 0 , BUFFER_LENGTH);
							n = recv(sockfd, buffer, BUFFER_LENGTH - 1, 0);
							if(n < 0) {
								error("ERROR reading from socket");
							}
							if(!n) {
								error("disconnected");
							}
							printf("server sent code %u, %d bytes read\n", (unsigned char)*buffer, n);
							print_reply_code_meaning((unsigned char)*buffer);
							if((unsigned char)*buffer == INTERNAL_SERVER_ERROR) {
								free(session_details->current_game->matrix);
								free(session_details->current_game);
								session_details->current_game = NULL;
								session_details->session_present = 0;
								break_loop = 1;
							}
							if(handler[(unsigned char)*buffer]) {
								ret_code = handler[(unsigned char)*buffer](buffer, &session_details);
								switch(ret_code) {
									case INTERNAL_CLIENT_ERROR: {
										fprintf(stderr, "error occured, exiting...\n");
										exit(ret_code);	
									} break;
									case INVALID_REQUEST: {
										fprintf(stderr, "invalid request\n");
										continue;
									}
									/*
									case GAME_IS_FINISHED: {
										if(session_details->current_game->character == session_details->current_game->state + 0x20) {
											printf("You won the game.\n");
										} else {
											printf("You lost the game.\n");
										}
										free(session_details->current_game->matrix);
										free(session_details->current_game);
										session_details->current_game = NULL;
									} break;
									*/
								}
							}
							if(!session_details->wait) {
								break;
							}
						}
						switch((unsigned char)*buffer) {
							case INTERNAL_SERVER_ERROR:
							case PEER_LEFT_NOTIFY: {
								print_reply_code_meaning((unsigned char)*buffer);
								free(session_details->current_game->matrix);
								free(session_details->current_game);
								session_details->current_game = NULL;
								session_details->session_present = 0;
								break_loop = 1;
							} break;
						}
					}
				} break;
			}
			print_board(session_details->current_game);
			if(break_loop) {
				break;
			}
		}
	}
	free(session_details->logged_in_user);
	free(session_details);
	close(sockfd);
	return 0;
}
