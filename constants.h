#define NUMBER_OF_OPCODES 256
#define BUFFER_LENGTH 256
#define USERNAMELEN 4
#define PASSWORDLEN 4
#define BOARD_SIZE 3
#define REALLOC_SIZE 5

enum {
	LOGIN_REQUEST,
	LOGOUT_REQUEST,
	CREATE_USER_REQUEST,
	JOIN_RANDOM_GAME_REQUEST,
	CREATE_NEW_GAME_REQUEST,
	LEAVE_GAME_REQUEST,
	ACTION_REQUEST,
	INTERNAL_CLIENT_ERROR
};

enum {
	PEER_LEFT_NOTIFY = 233,
	CANNOT_WRITE_HERE,
	ACTION_NOTIFY,
	OTHER_PLAYER_PRESENT_NOTIFY,
	NO_FURTHER_ACTIONS_PERMITTED,
	NOT_YOUR_TURN,
	GAME_IS_FINISHED,
	LEAVE_GAME_REPLY,
	JOIN_RANDOM_GAME_REPLY,
	ACTION_REPLY,
	CREATE_NEW_GAME_SUCCESS,
	CREATE_USER_SUCCESS,
	LOGIN_SUCCESS,
	LOGOUT_REPLY,
	NO_PLAYER_PRESENT, // begin non fatal
	INVALID_OPERANDS,
	NO_GAMES_AVAILABLE,
	NOT_IMPLEMENTED,
	USER_ALREADY_EXISTS,
	INTERNAL_SERVER_ERROR, // begin fatal
	INVALID_REQUEST,
	IO_ERROR,
	LOGIN_FAILED
};

enum {
	ANY_ERROR = NO_PLAYER_PRESENT, // begin error codes
	NO_ERROR = LOGOUT_REPLY,
	FATAL_ERRORS = INTERNAL_SERVER_ERROR
};
