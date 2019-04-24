#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "constants.h"

typedef struct usr {
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
} User;

struct game_board {
	char *matrix;
	User *player_1;
	User *player_2;
	User *host;
	char whose_turn;
	size_t board_size;
	size_t index;
	unsigned long player1_last_x, player1_last_y;
	unsigned long player2_last_x, player2_last_y;
	int player1_fd, player2_fd;
	pthread_mutex_t monitor;
};

struct game_boards_array {
	struct game_board **array;
	char *visited; // this array is used in selecting a random game to join
	size_t number_of_elements;
	size_t array_size;
	pthread_mutex_t monitor;
};

struct session_details {
	User *logged_in_user;
	size_t bytes_written; // number of bytes written after the last operation
	int fd;
	char session_present;
	struct game_board *current_game;
	struct game_boards_array *games;
};

struct arguments {
	int fd;
	struct game_boards_array *games;
};

unsigned char login_request(char*, struct session_details**);
unsigned char logout_request(char*, struct session_details**);
unsigned char create_user_request(char*, struct session_details**);
unsigned char create_new_game_request(char*, struct session_details**);
unsigned char join_random_game_request(char*, struct session_details**);
unsigned char leave_game_request(char*, struct session_details**);
unsigned char action_request(char*, struct session_details**);

static unsigned char (*handler[NUMBER_OF_OPCODES])(char*, struct session_details**) = {
	[LOGIN_REQUEST] =			login_request,	
	[LOGOUT_REQUEST] =			logout_request,
	[CREATE_USER_REQUEST] =			create_user_request,
	[CREATE_NEW_GAME_REQUEST] =		create_new_game_request,
	[JOIN_RANDOM_GAME_REQUEST] =		join_random_game_request,
	[LEAVE_GAME_REQUEST] = 			leave_game_request,
	[ACTION_REQUEST] =			action_request
};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

struct game_boards_array* array_of_games_init(const size_t size)
{
	struct game_boards_array *array = NULL;
	array = malloc(sizeof(struct game_boards_array));
	if(!array || !size) {
		free(array);
		return NULL;
	}
	array->array = NULL;
	array->array_size = size;
	array->number_of_elements = 0;
	array->visited = NULL;
	array->array = calloc(size, sizeof(struct game_board*));
	if(!array->array || pthread_mutex_init(&array->monitor, NULL)) {
		free(array);
		return NULL;
	}
	return array;
}

void game_boards_array_free(struct game_boards_array *ptr)
{
	if(!ptr) {
		return;
	}
	for(size_t i = 0; i < ptr->array_size; ++i) {
		free(ptr->array[i]->matrix);
		pthread_mutex_destroy(&ptr->array[i]->monitor);
		free(ptr->array[i]);
	}
	pthread_mutex_destroy(&ptr->monitor);
	free(ptr->visited);
	free(ptr->array);
	free(ptr);
}

int game_boards_array_add(struct game_boards_array *array, struct game_board *game)
{
	if(!array || !game) {
		return -3;
	}
	int ret_value;
	if((ret_value = pthread_mutex_lock(&array->monitor))) {
		return ret_value;
	}
	if(!array->array_size) {
		pthread_mutex_unlock(&array->monitor);
		return -2;
	}
	if(array->number_of_elements == array->array_size) {
		pthread_mutex_unlock(&array->monitor);
		return -1;
	}
	struct game_board **new;
	char *visited_new;
	game->index = array->number_of_elements;
	if(!array->visited) { // first add to array
		array->visited = calloc(1, 1);
		if(!array->visited) {
			pthread_mutex_unlock(&array->monitor);
			return -4;
		}
	}
	array->array[array->number_of_elements++] = game;
	if(array->number_of_elements == array->array_size) {
		new = realloc(array->array, sizeof(struct game_board*) * (array->array_size + REALLOC_SIZE));
		visited_new = realloc(array->visited, array->number_of_elements);
		if(!new || !visited_new) {// in case of realloc error, the pointer remains valid, not null 
			array->number_of_elements--;
			pthread_mutex_unlock(&array->monitor);
			return -4;
		}
		array->array_size += REALLOC_SIZE;
		array->array = new;
		visited_new[array->number_of_elements - 1] = 0;
		array->visited = visited_new;
		memset(new + array->number_of_elements, 0, REALLOC_SIZE);
	}
	return pthread_mutex_unlock(&array->monitor);
}

int game_boards_array_remove(struct game_boards_array *array, struct game_board* game)
{
	if(!game || !array) {
		return 3;
	}
	int ret_value;
	if((ret_value = pthread_mutex_lock(&array->monitor))) {
		return ret_value;
	}
	size_t index = game->index;
	if(!array->number_of_elements || array->number_of_elements - 1 < index) {
		printf("on remove %lu %lu\n", array->number_of_elements, index);
		pthread_mutex_unlock(&array->monitor);
		return 2;
	}
	char *visited_new;
	struct game_board **array_new;
	free(array->array[index]->matrix);
	if((ret_value = pthread_mutex_destroy(&array->array[index]->monitor))) {
		pthread_mutex_unlock(&array->monitor);
		return ret_value;
	}
	if(index < array->number_of_elements - 1) { // this comparison is probably redundant
		array->array[index] = array->array[array->number_of_elements - 1]; // move the last element to the deleted one's place
		array->array[index]->index = index; // update the moved element's index
	}
	array->array[array->number_of_elements-- - 1] = NULL;
	free(array->array[index]);
	// begin questionable realloc
	if(array->array_size - array->number_of_elements == REALLOC_SIZE * 2 && array->array_size > REALLOC_SIZE * 2) {
		array_new = realloc(array->array, sizeof(struct game_board*) * array->array_size);
		if(!array_new) {
			pthread_mutex_unlock(&array->monitor);
			return -1;
		}
		array->array_size -= REALLOC_SIZE * 2;
		array->array = array_new;
	}
	if(array->number_of_elements) {
		visited_new = realloc(array->visited, array->number_of_elements);
		if(!visited_new) {
			pthread_mutex_unlock(&array->monitor);
			return -1;
		}
		array->visited = visited_new;
		memset(array->visited, 0, array->number_of_elements);
	} else {
		free(array->visited);
		array->visited = NULL;
	}
	printf("removed %p\n", game);
	return pthread_mutex_unlock(&array->monitor);
}

void error(const char *msg)
{
	perror(msg);
	exit(1);
}

/*
 * the following code is suboptimal, to put it mildly. and it's quite poorly designed. TODO
 * */
int write_x_or_o(struct game_board *board, size_t x, size_t y, const char character)
{
	if(!board || !character) {
	 	return -1;
	}
	if(board->whose_turn != character) {
		printf("return -4 %c %c\n", board->whose_turn, character);
		return -4;
	}
	size_t i, j;
	if(x > (board->board_size - 1) || y > (board->board_size - 1)) { // behaviour is undefined is size happens to be zero
		printf("return -2\n");
		return -2;
	} // matrix indexing looks ugly because it's a 1d array for more efficiency... (1 pointer dereference fewer)
	if(board->matrix[(board->board_size * x) + y] != 'x' && board->matrix[(board->board_size * x) + y] != 'o' && (character == 'x' || character == 'o')) {
		printf("write\n");
		board->matrix[(board->board_size * x) + y] = character;	
	} else {
		printf("return -5\n");
		return -5;
	}
	for(i = 0; i < board->board_size; i++) {
		if(board->matrix[(board->board_size * x) + i] != character) {
			break;
		}
	}
	if(i == board->board_size) {
		board->whose_turn = character - 0x20; // make it uppercase, which means the game is over
		return 0; // the one who wrote won the game 
	} 
	for(i = 0; i < board->board_size; i++) {
		if(board->matrix[(board->board_size * i) + y] != character) {
			break;
		}
	}
	if(i == board->board_size) {
		board->whose_turn = character - 0x20;
		return 0;
	}
	if(x == y || y == (board->board_size - x)) {
		for(i = 0; i < board->board_size; i++) {
			if(board->matrix[(board->board_size * i) + i] != character) {
				break;
			}
		}
		if(i == board->board_size) {
			board->whose_turn = character - 0x20;
			return 0;
		}
		j = board->board_size - 1;
		for(i = 0; i < board->board_size && j; i++) {
			if(board->matrix[(board->board_size * i) + j] != character) {
				break;
			}
			--j;
		}
		if(!j && board->matrix[(board->board_size * (i - 1)) + j] == character) { // the last comparison is outside the loop to prevent j's underflow
			board->whose_turn = character - 0x20;
			return 0;
		}
	}
	for(i = 0; i < board->board_size; ++i)
		for(j = 0; j < board->board_size; ++j) {
			if(board->matrix[(board->board_size * i) + j] == ' ') {
				break;
			}
		}
	if(i == board->board_size && j == board->board_size) {
		printf("tie\n");
		board->whose_turn = 'D';
	} else {
		board->whose_turn = character == 'x' ? 'o' : 'x';
	}
	return 0;
}

/*
 * causes undefined behaviour if buffer_size is longer than the buffer itself
 * */
char* find_character_in_buffer(char *buffer, size_t buffer_size, const char character)
{
	if(!buffer || !buffer_size) {
		return NULL;
	}
	for(size_t i = 0; i < buffer_size; i++) {
		if(buffer[i] == character) {
			return buffer + i;
		}
	}
	return NULL;
}

User* find_user_by_name(FILE *input_file, char* name)
{
	if(!input_file || !name) {
		return NULL;
	}
	char *password_start;
	char string[USERNAMELEN + PASSWORDLEN + 3];
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
	User *found_user = NULL;
	memset(username, 0, USERNAMELEN + 1);
	memset(password, 0, PASSWORDLEN + 1);
	memset(string, 0, USERNAMELEN + PASSWORDLEN + 3);
	while((fgets(string, USERNAMELEN + PASSWORDLEN + 3, input_file))) {
		if(strncmp(strncpy(username, string, USERNAMELEN), name, USERNAMELEN)) {
			continue;
		}
		password_start = find_character_in_buffer(string, USERNAMELEN + PASSWORDLEN + 3, ' ');
		if(password_start) {
			// pointer arithmetic in order to check buffer bounds
			if((password_start - string) + 1 >= USERNAMELEN + PASSWORDLEN + 3) {
				break;
			}
			password_start++;
		} else {
			break;
		}
		strncpy(password, password_start, PASSWORDLEN);
		found_user = malloc(sizeof(User));
		if(found_user) {
			memcpy(found_user->username, username, USERNAMELEN + 1);
			memcpy(found_user->password, password, PASSWORDLEN + 1);
		} else {
			fprintf(stderr, "error: cannot allocate %lu bytes\n", sizeof(User));
			exit(1);
		}
		break;
	}
	fclose(input_file);
	return found_user;
}

int create_new_user(char *username, char *password)
/* TODO: outdated comment 
 * this function returns a positive value if there's a problem with locking or unlocking the mutex.
 * in case of some other  problem, e. g. i/o error, it returns a negative value.
 * */
{
	if(!username || !password) {
		return -3;
	}
	User *already_existing;
	int ret_value;
	if((ret_value = pthread_mutex_lock(&mutex))) {
		return ret_value;
	}
	if((already_existing = find_user_by_name(fopen("users.txt", "r"), username))) {
		pthread_mutex_unlock(&mutex);
		free(already_existing);
		return -2;
	}
	FILE *list = fopen("users.txt", "a+");
	if(!list) {
		pthread_mutex_unlock(&mutex);
		return -1;	
	}
	fprintf(list, "%s %s\n", username, password);
	fclose(list);
	return pthread_mutex_unlock(&mutex);
}

int parse_operands_from_buffer(char *operand1, char *operand2, char *buffer, const char separator, size_t buffer_size)
{
	if(!operand1 || !operand2 || !buffer || !buffer_size) {
		return -1;
	}
	memset(operand1, 0, buffer_size);
	memset(operand2, 0, buffer_size);
	char *op2_start = find_character_in_buffer(buffer, buffer_size, separator);
	if(op2_start) {
		 // checking buffer size with pointer arithmetic
		if((size_t)((op2_start - buffer) + 1) >= buffer_size) {
			return 2;
		}
		op2_start++;
	} else {
		return 1;
	} // this might break if the separator is not \0
	strncpy(operand1, buffer, buffer_size - 1);
	strncpy(operand2, op2_start, buffer_size - 1);
	return 0;
}

int get_coordinates_from_buffer(char *buffer, unsigned long *x, unsigned long *y)
{
	if(!x || !y) {
		return 1;
	}
	char *next = NULL;
	if(buffer[0] == '0' && buffer[1] == '\0') {
		*x = 0;
		next = buffer + 1;
	} else {
		*x = strtoul(buffer, &next, 10);
		if(!*x || errno == ERANGE) {
			return 2;
		}
	}
	if(next[1] == '0' && next[2] == '\0') {
		*y = 0;
	} else {
		*y = strtoul(++next, NULL, 10);
		if(!*y || errno == ERANGE) {
			return 3;
		}
	}
	printf("coor: %lu %lu\n", *x, *y);
	return 0;
}

unsigned char create_new_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	if(!(*session_details)->session_present || (*session_details)->current_game) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	(*session_details)->current_game = calloc(1, sizeof(struct game_board));
	if(!(*session_details)->current_game) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	struct game_board *game = (*session_details)->current_game;
	char to_uppercase = random() % 2 ? 0x20 : 0;
	game->board_size = BOARD_SIZE; // not hardcoded board size maybe?
	game->matrix = malloc(game->board_size * game->board_size);
	game->player_1 = NULL;
	game->player_2 = NULL;
	if(!game->matrix || pthread_mutex_init(&game->monitor, NULL)) {
		free(game);
		(*session_details)->current_game = NULL;
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	memset(game->matrix, ' ', game->board_size * game->board_size); // empty cells are spaces
	game->player1_fd = -1; // initialize fds to unusable values
	game->player2_fd = -1;
	if(random() % 2) {
		game->player_1 = (*session_details)->logged_in_user;
		game->player1_fd = (*session_details)->fd;
	} else {
		game->player_2 = (*session_details)->logged_in_user;
		game->player2_fd = (*session_details)->fd;
	}
	game->host = (*session_details)->logged_in_user;
	memset(buffer + 2, 0, BUFFER_LENGTH - 2);
	buffer[0] = CREATE_NEW_GAME_SUCCESS;
	buffer[1] = game->player_1 ? 'x' : 'o' - to_uppercase; // inform the client which player he is 
	switch(buffer[1]) {
		case 'O': case 'x': game->whose_turn = 'o'; break;
		case 'X': case 'o': game->whose_turn = 'x'; break;
	}
	int bytes_written = snprintf(buffer + 2, BUFFER_LENGTH - 2, "%lu", game->board_size); //count not including null terminator
	if(bytes_written > 0 && !game_boards_array_add((*session_details)->games, game)) {
		(*session_details)->bytes_written = 3 + bytes_written;  // three first buffer bytes and a null terminator
	} else {
		free(game);
		(*session_details)->current_game = NULL;
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	printf("fd %d created a game, %c\n", game->player1_fd < 0 ? game->player2_fd : game->player1_fd, buffer[1]);
	return CREATE_NEW_GAME_SUCCESS;
}

unsigned char join_random_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	if(!(*session_details)->session_present || (*session_details)->current_game) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	if(pthread_mutex_lock(&(*session_details)->games->monitor)) { 
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	struct game_boards_array *games = (*session_details)->games;
	size_t roll, roll_prev, size, i = 0;
	char which, to_uppercase;
	size = games->number_of_elements;
	roll_prev = UINT_MAX; // initial value for more elegant loop
	for(;;) {
		if(i == size) {
			buffer[0] = NO_GAMES_AVAILABLE;
			(*session_details)->bytes_written = 1;
			memset(games->visited, 0, games->number_of_elements);
			if(pthread_mutex_unlock(&games->monitor)) {
				buffer[0] = INTERNAL_SERVER_ERROR;
				(*session_details)->bytes_written = 1;
				return INTERNAL_SERVER_ERROR;
			}
			return NO_GAMES_AVAILABLE;
		}
		roll = random() % size;
		if(roll == roll_prev || games->visited[roll]) {
			continue;
		}
		games->visited[roll] = 1;
		++i;
		if(!games->array[roll]->player_1) {
			games->array[roll]->player_1 = (*session_details)->logged_in_user;
			games->array[roll]->player1_fd = (*session_details)->fd;
			printf("fd %d joined\n", (*session_details)->fd);
			to_uppercase = games->array[roll]->whose_turn == 'x' ? 0x20 : 0;
			which = 0;
			break;
		} else if(!games->array[roll]->player_2) {
			games->array[roll]->player_2 = (*session_details)->logged_in_user;
			games->array[roll]->player2_fd = (*session_details)->fd;
			printf("fd %d joined\n", (*session_details)->fd);
			to_uppercase = games->array[roll]->whose_turn == 'o' ? 0x20 : 0;
			which = 1;
			break;
		}
		roll_prev = roll;
	}
	memset(games->visited, 0, games->number_of_elements);
	(*session_details)->current_game = games->array[roll]; 
	buffer[0] = JOIN_RANDOM_GAME_REPLY;
	buffer[1] = !which ? 'x' : 'o';
	/*
	if(((*session_details)->current_game->player_1 == (*session_details)->logged_in_user && (*session_details)->current_game->whose_turn == 'x')
	|| ((*session_details)->current_game->player_2 == (*session_details)->logged_in_user && (*session_details)->current_game->whose_turn == 'o')) {
		to_uppercase = 0x20;
	}
	*/
	buffer[1] -= to_uppercase; //upppercase indicates that this player will begin the game
	printf("join %c\n", buffer[1]);
	int bytes_written = snprintf(buffer + 2, BUFFER_LENGTH - 2, "%lu", (*session_details)->current_game->board_size);
	if(pthread_mutex_unlock(&games->monitor)) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	if(bytes_written > 0) {
		(*session_details)->bytes_written = 3 + bytes_written; 
	} else {
		(*session_details)->current_game = NULL;
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	return JOIN_RANDOM_GAME_REPLY;
}

unsigned char leave_game_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	if(!(*session_details)->session_present || !(*session_details)->current_game) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	if(pthread_mutex_lock(&(*session_details)->current_game->monitor)) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	struct game_board *game = (*session_details)->current_game;
	int ret_value;
	game->whose_turn = 0;
	if(game->host == (*session_details)->logged_in_user) {
		game->host = NULL;
	}
	if((*session_details)->logged_in_user == game->player_1) {
		game->player_1 = NULL;
	} else if((*session_details)->logged_in_user == game->player_2) {
		game->player_2 = NULL;
	}
	ret_value = pthread_mutex_unlock(&game->monitor);
	if(ret_value || (!game->player_1 && !game->player_2 && game_boards_array_remove((*session_details)->games, game))) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	} else {
		printf("remove 1\n");
	}
	(*session_details)->current_game = NULL;
	buffer[0] = LEAVE_GAME_REPLY;
	(*session_details)->bytes_written = 1;
	return LEAVE_GAME_REPLY;
}

unsigned char action_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	if(!(*session_details)->session_present || !(*session_details)->current_game) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	if(pthread_mutex_lock(&(*session_details)->current_game->monitor)) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	if(!(*session_details)->current_game->whose_turn || (*session_details)->current_game->whose_turn == 'D') {
		buffer[0] = NO_FURTHER_ACTIONS_PERMITTED;
		(*session_details)->bytes_written = 1;
		if(pthread_mutex_unlock(&(*session_details)->current_game->monitor)) {
			buffer[0] = INTERNAL_SERVER_ERROR;
			return INTERNAL_SERVER_ERROR;
		}
		return NO_FURTHER_ACTIONS_PERMITTED;
	}
	if(!(*session_details)->current_game->player_1 || !(*session_details)->current_game->player_2) {
		buffer[0] = NO_PLAYER_PRESENT;
		(*session_details)->bytes_written = 1;
		if(pthread_mutex_unlock(&(*session_details)->current_game->monitor)) {
			buffer[0] = INTERNAL_SERVER_ERROR;
			return INTERNAL_SERVER_ERROR;
		}
		return NO_PLAYER_PRESENT;
	}
	if(((*session_details)->current_game->player_1 == (*session_details)->logged_in_user && (*session_details)->current_game->whose_turn == 'o')
	|| ((*session_details)->current_game->player_2 == (*session_details)->logged_in_user && (*session_details)->current_game->whose_turn == 'x')) {
		buffer[0] = NOT_YOUR_TURN;
		(*session_details)->bytes_written = 1;
		if(pthread_mutex_unlock(&(*session_details)->current_game->monitor)) {
			buffer[0] = INTERNAL_SERVER_ERROR;
			return INTERNAL_SERVER_ERROR;
		}
		return NOT_YOUR_TURN;
	}
	char character = (*session_details)->current_game->player_1 == (*session_details)->logged_in_user ? 'x' : 'o'; //player 1 draws x
	unsigned long x, y;
	int ret_value;
	if(get_coordinates_from_buffer(buffer + 1, &x, &y)) {
		buffer[0] = INVALID_OPERANDS;
		(*session_details)->bytes_written = 1;
		return INVALID_OPERANDS;
	}
	ret_value = write_x_or_o((*session_details)->current_game, x, y, character);
	switch(ret_value) {
		case -2: {
			pthread_mutex_unlock(&(*session_details)->current_game->monitor);
			buffer[0] = INVALID_OPERANDS;
			(*session_details)->bytes_written = 1;
			printf("hello operands 2\n");
			return INVALID_OPERANDS;
		}
		case -5: {
			pthread_mutex_unlock(&(*session_details)->current_game->monitor);
			buffer[0] = CANNOT_WRITE_HERE;
			(*session_details)->bytes_written = 1;
			return CANNOT_WRITE_HERE;
		}
		case 0: break;
		case -1:
		case -4: break;
		default: {
			pthread_mutex_unlock(&(*session_details)->current_game->monitor);
			buffer[0] = INTERNAL_SERVER_ERROR;
			(*session_details)->bytes_written = 1;
			printf("hello internal 3, %d\n", ret_value);
			return INTERNAL_SERVER_ERROR;
		}
	}
	if((*session_details)->logged_in_user == (*session_details)->current_game->player_1) {
		(*session_details)->current_game->player1_last_x = x;
		(*session_details)->current_game->player1_last_y = y;
	} else {
		(*session_details)->current_game->player2_last_x = x;
		(*session_details)->current_game->player2_last_y = y;
	}
	if((*session_details)->current_game->whose_turn == 'X' || (*session_details)->current_game->whose_turn == 'O'
	|| (*session_details)->current_game->whose_turn == 'D') {
		buffer[0] = GAME_IS_FINISHED;
		buffer[1] = (*session_details)->current_game->whose_turn;
		(*session_details)->bytes_written = 2;
		if(pthread_mutex_unlock(&(*session_details)->current_game->monitor)) {
			buffer[0] = INTERNAL_SERVER_ERROR;
			(*session_details)->bytes_written = 1;
			return INTERNAL_SERVER_ERROR;
		}
		return GAME_IS_FINISHED;
	}
	if(pthread_mutex_unlock(&(*session_details)->current_game->monitor)) {
		buffer[0] = INTERNAL_SERVER_ERROR;
		(*session_details)->bytes_written = 1;
		return INTERNAL_SERVER_ERROR;
	}
	buffer[0] = ACTION_REPLY;
	(*session_details)->bytes_written = 1;
	return ACTION_REPLY;
}
unsigned char logout_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	} else if(!(*session_details)->session_present) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	(*session_details)->session_present = 0;
	buffer[0] = LOGOUT_REPLY;
	(*session_details)->bytes_written = 1;
	return LOGOUT_REPLY;
}

unsigned char login_request(char *buffer, struct session_details **session_details)
{
	if(!*session_details) {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	if(*session_details && (*session_details)->session_present) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	}
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
	memset(username, 0, USERNAMELEN + 1);
	memset(password, 0, PASSWORDLEN + 1);
	strncpy(username, buffer + 1, USERNAMELEN);
	if(pthread_mutex_lock(&mutex)) {
		(*session_details)->bytes_written = 1;
		(*session_details)->session_present = 0;
		buffer[0] = INTERNAL_SERVER_ERROR;
		return INTERNAL_SERVER_ERROR;
	}
	(*session_details)->logged_in_user = find_user_by_name(fopen("users.txt", "r"), username);
	if(pthread_mutex_unlock(&mutex)) {
		(*session_details)->bytes_written = 1;
		(*session_details)->session_present = 0;
		buffer[0] = INTERNAL_SERVER_ERROR;
		return INTERNAL_SERVER_ERROR;
	}
	if(!(*session_details)->logged_in_user) {
		buffer[0] = LOGIN_FAILED;
		(*session_details)->bytes_written = 1;
		(*session_details)->session_present = 0;
		return LOGIN_FAILED; 
	}
	strncpy(password, buffer + strlen(username) + 2, PASSWORDLEN);
	if(strncmp(password, (*session_details)->logged_in_user->password, PASSWORDLEN)) {
		buffer[0] = LOGIN_FAILED;
		(*session_details)->bytes_written = 1;
		(*session_details)->session_present = 0;
		return LOGIN_FAILED; 
	}
	(*session_details)->session_present = 1;
	buffer[0] = LOGIN_SUCCESS;
	(*session_details)->bytes_written = 1;
	return LOGIN_SUCCESS;
}

unsigned char create_user_request(char *buffer, struct session_details **session_details)
{
	if(*session_details && (*session_details)->session_present) {
		buffer[0] = INVALID_REQUEST;
		(*session_details)->bytes_written = 1;
		return INVALID_REQUEST;
	} else {
		buffer[0] = INVALID_REQUEST;
		return INVALID_REQUEST;
	}
	char username[USERNAMELEN + 1];
	char password[PASSWORDLEN + 1];
	strncpy(username, buffer + 1, USERNAMELEN);
	strncpy(password, buffer + strlen(username) + 2, PASSWORDLEN); // vulnerable to buffer overflow, TODO fix
	int ret = create_new_user(username, password);
	switch(ret) {
		case -1: case 1: {
			buffer[0] = INTERNAL_SERVER_ERROR;
			(*session_details)->bytes_written = 1;
		} break;
		case 2: {
			buffer[0] = USER_ALREADY_EXISTS;
			(*session_details)->bytes_written = 1;
			return USER_ALREADY_EXISTS;
		}
		case 0: {
			buffer[0] = CREATE_USER_SUCCESS;
			(*session_details)->bytes_written = 1;
			//(*session_details)->session_present = 1;
			(*session_details)->logged_in_user = calloc(1, sizeof(User));
			if(!(*session_details)->logged_in_user) {
				free(*session_details);
				*session_details = NULL;
				break;
			}
			strncpy((*session_details)->logged_in_user->username, username, USERNAMELEN);
			strncpy((*session_details)->logged_in_user->password, password, PASSWORDLEN);
			return CREATE_USER_SUCCESS;
		}
	}
	return INTERNAL_SERVER_ERROR;
}

void* connection_handler(void *arg)
{
	unsigned long last_x, last_y;
	char buffer[BUFFER_LENGTH];
	int n, n2, peer_fd, ret_value;
	unsigned char return_code, opcode;
	struct arguments *arguments = (struct arguments*) arg;
	int fd = arguments->fd;
	struct game_boards_array *games = arguments->games;
	struct session_details *session_details = NULL;
	size_t bytes_written;
	session_details = calloc(1, sizeof(struct session_details));
	if(!session_details) {
		free(arg);
		close(fd);
		return NULL;
	}
	session_details->games = games;
	session_details->current_game = NULL;
	memset(buffer, 0, BUFFER_LENGTH);
	session_details->fd = fd;
	n = recv(fd, buffer, BUFFER_LENGTH - 1, 0);
	if(n < 0) {
		perror("error on recv");
		return NULL;
	} else if(!n) {
		shutdown(fd, SHUT_RDWR);
		free(arg);
		return NULL;
	}
	if(buffer[0] != LOGIN_REQUEST && buffer[0] != CREATE_USER_REQUEST) {
		return_code = INVALID_REQUEST;
		n2 = send(fd, &return_code, 1, MSG_NOSIGNAL);
		if(n2 < 0) {
			perror("error on send");
		}
		close(fd);
		free(arg);
		return NULL;
	} else {
		opcode = (unsigned char) buffer[0];
		return_code = handler[opcode](buffer, &session_details);	
		bytes_written = session_details->bytes_written;
		n2 = send(fd, buffer, bytes_written, MSG_NOSIGNAL);
		if(n2 < 0) {
			perror("error on send");
			free(arg);
			return NULL;
		}
		if((return_code >= FATAL_ERRORS)) {
			free(session_details->logged_in_user);
			free(session_details);
			session_details = NULL;
		}
		printf("end login %u\n", return_code);
	}
	while(session_details && session_details->session_present) {
		memset(buffer, 0, BUFFER_LENGTH);
		n = recv(fd, buffer, BUFFER_LENGTH - 1, 0);
		if(n <= 0) {
			perror("error on recv");
			if(session_details->current_game) {
				printf("remove2\n");
				if((ret_value = game_boards_array_remove(session_details->games, session_details->current_game))) {
					fprintf(stderr, "error on remove? %d\n", ret_value);
				}
				peer_fd = fd == session_details->current_game->player1_fd ?	session_details->current_game->player2_fd : 
												session_details->current_game->player1_fd; 
				printf("sending leave notify\n");
				buffer[0] = PEER_LEFT_NOTIFY;
				n = send(peer_fd, buffer, 1, MSG_NOSIGNAL);
				if(n < 0) {
					fprintf(stderr, "error on sending peer left notify\n");
				}
			}
			break;
		}
		opcode = (unsigned char) buffer[0];
		if(handler[opcode]) {
			return_code = handler[opcode](buffer, &session_details);
			bytes_written = session_details->bytes_written;
			if((return_code >= FATAL_ERRORS)) {
				free(session_details->logged_in_user);
				free(session_details);
				session_details = NULL;
			}
			n2 = send(fd, buffer, bytes_written, MSG_NOSIGNAL);
			if(n2 < 0) {
				perror("error on send");
				break;
			}
			switch((unsigned char)*buffer) {
				case JOIN_RANDOM_GAME_REPLY: {
					peer_fd = session_details->current_game->host == session_details->current_game->player_1 ?
						session_details->current_game->player1_fd :
						session_details->current_game->player2_fd;
					buffer[0] = OTHER_PLAYER_PRESENT_NOTIFY;
					n2 = send(peer_fd, buffer, 1, MSG_NOSIGNAL);
					if(n2 < 0) {
						perror("error on send");
						break;
					}
				} break;
				case ACTION_REPLY: {
					printf("sending notify...\n");
					int count1, count2 = -1;
					char *next = NULL;
					memset(buffer, 0, BUFFER_LENGTH);
					buffer[0] = ACTION_NOTIFY;
					buffer[1] = session_details->current_game->whose_turn;
					if(session_details->current_game->whose_turn == 'o') {
						last_x = session_details->current_game->player1_last_x;
						last_y = session_details->current_game->player1_last_y;
						peer_fd = session_details->current_game->player2_fd;
					} else if(session_details->current_game->whose_turn == 'x') {
						last_x = session_details->current_game->player2_last_x;
						last_y = session_details->current_game->player2_last_y;
						peer_fd = session_details->current_game->player1_fd;
					}
					printf("last %lu %lu\n", last_x, last_y);
					count1 = snprintf(buffer + 2, BUFFER_LENGTH / 2, "%lu", last_x);
					if(count1 > 0) {
						next = buffer + count1 + 3;
						count2 = snprintf(next, BUFFER_LENGTH / 2, "%lu", last_y);
					}
					if(count1 <= 0 || count2 <= 0) {
						perror("error on sending notify");
					}
					bytes_written = 2 + count1 + count2 + 2;
					n2 = send(peer_fd, buffer, bytes_written, MSG_NOSIGNAL);
					if(n2 < 0) {
						perror("error on send");
					}
					printf("sent to %d\n", peer_fd);
				} break;
				case GAME_IS_FINISHED: {
					buffer[0] = GAME_IS_FINISHED;
					buffer[1] = session_details->current_game->whose_turn;
					n = send(peer_fd, buffer, 2, MSG_NOSIGNAL);
				} break;
			}
		} else {
			buffer[0] = NOT_IMPLEMENTED;
			n2 = send(fd, buffer, 1, MSG_NOSIGNAL);
			if(n2 < 0) {
				perror("error on send");
				break;
			}
		}
	}
	printf("end connection\n");
	free(arg);
	free(session_details);
	shutdown(fd, SHUT_RDWR);
	return NULL;
}

int main(int argc, char **argv)
{
	int sockfd, newsockfd, portno;
	pthread_t thread;
	pthread_attr_t attributes;
	socklen_t clilen;
	struct sockaddr_in serv_addr, cli_addr;
	struct game_boards_array *games = array_of_games_init(REALLOC_SIZE);
	struct arguments *arg;
	if(!games) {
		error("error on mallocing stuff");
	}
	//signal(SIGINT, sigint_handler);
	if(argc < 2) {
		fprintf(stderr,"ERROR, no port provided\n");
		exit(1);
	}
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd < 0) {
		error("ERROR opening socket");
	}
	memset((char *) &serv_addr, 0,  sizeof(serv_addr));
	portno = atoi(argv[1]);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);
	if(bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		error("ERROR on binding");
	}
	listen(sockfd, 5);
	clilen = sizeof(cli_addr);
	if(pthread_attr_init(&attributes)) {
		error("error initialising thread attributes structure");
	}
	if(pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED)) {
		error("error setting thread attribute to detached state");
	}
	srandom(time(NULL));
	for(;;) {
		newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		if (newsockfd < 0) {
			error("error on accept");
		}
		printf("Got a connection from %s on port %d\n", inet_ntoa(cli_addr.sin_addr), htons(cli_addr.sin_port));
		//int *copy_newsockfd = malloc(sizeof(int));
		arg = malloc(sizeof(struct arguments));
		if(arg) {
			arg->fd = newsockfd;	
			arg->games = games;
			if(pthread_create(&thread, &attributes, connection_handler, arg)) {
				fprintf(stderr, "failed to create thread\n");	
				free(arg);
				arg = NULL;
			}
		} else {
			error("error on malloc");
		}
	}
	game_boards_array_free(games);
	pthread_attr_destroy(&attributes);
	close(sockfd);
	return 0; 
}
