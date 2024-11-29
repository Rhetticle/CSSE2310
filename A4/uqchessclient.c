#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <semaphore.h>
#include <ctype.h>
#include <signal.h>
#include <csse2310a4.h>

// Minimum number of args to program including program name itself
#define MIN_CMDLINE_ARG_COUNT 2

// Maximum and minimum length of a move string e.g. "e2e4"
#define MIN_MOVE_STRING_LENGTH 4
#define MAX_MOVE_STRING_LENGTH 5

// Error/status codes
#define STATUS_OK 0
#define USAGE_ERROR 4
#define PORT_ERROR 18
#define SERVER_ERROR 5

// Error strings for different error cases
const char* const invalidCmdError = "Command is not valid - try again\n";
const char* const gameNotStartedError
        = "Command not valid - game is not in progress\n";
const char* const turnError = "Command is not valid - it's not your turn\n";

/* Parameters
 *
 * Stores information about arguments passed to program from command line.
 *
 * port - Port to connect on
 * opponent - String representation of opponent (e.g. "human" or "computer")
 * colour - String representation of colour being played ("black" or "white")
 */
typedef struct {
    char* port;
    char* opponent;
    char* colour;
} Parameters;

/* GameInfo
 *
 * Stores information about the current game being played as well as other
 * information needed by the worker threads.
 *
 * infoLock: Semaphore to use when updating values within struct
 * turn: true if it is the clients turn to play, false otherwise
 * started: true if the client is currently in a game, false otherwise
 * sockfd: File descriptor of socket connected to server
 * colour: Colour being played by client
 * opponent: Opponent of client (human or computer)
 */
typedef struct {
    sem_t* infoLock;
    bool turn;
    bool started;
    int sockfd;
    char* colour;
    char* opponent;
} GameInfo;

/* free_parameters()
 * -----------------
 * Free a Paramters type.
 *
 * param: Parameters struct to free
 */
void free_parameters(Parameters* param)
{
    if (param->port) {
        free(param->port);
    }
    if (param->colour) {
        free(param->colour);
    }
    if (param->opponent) {
        free(param->opponent);
    }
    free(param);
}

/* free_game_info()
 * ----------------
 * Free a GameInfo struct.
 *
 * info: GameInfo struct to free
 */
void free_game_info(GameInfo* info)
{
    sem_destroy(info->infoLock);
    free(info->colour);
    free(info->opponent);
    free(info);
}

/* arg_is_option()
 * ---------------
 * Check if a command line string is an option (begins with "--").
 *
 * arg: Argument to check if it is an option or not
 *
 * Returns: true if arg begins with "--", false otherwise.
 */
bool arg_is_option(const char* arg)
{
    if ((arg[0] == '-') && (arg[1] == '-')) {
        return true;
    }
    return false;
}

/* parse_option()
 * --------------
 * Parse an option argument appropriately into a Parameters struct param.
 *
 * param: Parameters struct to parse option into
 * argc: Number of command line arguments
 * argv: String representation of command line arguments
 * index: Pointer to index of argv at which the option being parsed is
 *
 * Returns: true if option was valid and could be parsed into param, false
 *          otherwise.
 */
bool parse_option(Parameters* param, int argc, char** argv, int* index)
{
    if ((*index == argc) || (!argv[*index + 1][0])) {
        return false;
    }
    // get the option
    char* option = argv[*index];

    // check that if "--play" has been specified that this is the first
    // occurence
    if ((!strcmp(option, "--play")) && (!param->opponent)) {
        // set param->opponent string depending on what follows "--play"
        if (!strcmp(argv[*index + 1], "human")) {
            param->opponent = strdup("human");
        } else if (!strcmp(argv[*index + 1], "computer")) {
            param->opponent = strdup("computer");
        } else {
            return false;
        }
        // increment index to skip over what followed "--play"
        (*index)++;
    } else if ((!strcmp(option, "--col")) && (!param->colour)) {
        // set param->colour depending on what follows "--col" on commandline
        if (!strcmp(argv[*index + 1], "black")) {
            param->colour = strdup("black\n");
        } else if (!strcmp(argv[*index + 1], "white")) {
            param->colour = strdup("white\n");
        } else {
            return false;
        }
        // increment index to skip over what followed "--col"
        (*index)++;
    } else {
        return false;
    }
    return true;
}

/* process_cmdline_args()
 * ----------------------
 * Process the command line arguments passed to program into a Parameters
 * struct.
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 *
 * Returns: Pointer to Parameters struct containing relevant information passed
 *          through command line OR NULL if the arguments passed were invalid.
 */
Parameters* process_cmdline_args(int argc, char** argv)
{
    // not enough arguments given (need at least a port)
    if ((argc < MIN_CMDLINE_ARG_COUNT) || (arg_is_option(argv[1]))) {
        return NULL;
    }
    // use calloc to initialise param pointers to NULL
    Parameters* param = calloc(1, sizeof(Parameters));

    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) {
            // argument was empty string
            return NULL;
        }
        if (arg_is_option(argv[i]) && (i != argc - 1)) {
            // argument is an option so call parse_option()
            if (!parse_option(param, argc, argv, &i)) {
                free_parameters(param);
                return NULL;
            }
        } else {
            // not an option so must be port
            if (param->port == NULL) {
                param->port = strdup(argv[i]);
            } else {
                free_parameters(param);
                return NULL;
            }
        }
    }
    if (!param->port) {
        // didn't get a port
        free_parameters(param);
        return NULL;
    }
    // param->opponent hasn't been set so initialise to default
    if (!param->opponent) {
        param->opponent = strdup("computer");
    }
    // param->colour hasn't been set so initialise to default
    if (!param->colour) {
        if (!strcmp(param->opponent, "human")) {
            param->colour = strdup("either\n");
        } else {
            param->colour = strdup("white\n");
        }
    }
    return param;
}

/* connection_error()
 * ------------------
 * Print a connection error string to stderr.
 *
 * port: Port being attempted to connect to
 */
void connection_error(char* port)
{
    fprintf(stderr, "uqchessclient: can't make connection to port \"%s\"\n",
            port);
    fflush(stderr);
}

/* command_error()
 * ---------------
 * Print a command error string to stderr due to an invalid command from user.
 *
 * cmdErrString: Error string to print
 */
void command_error(const char* const cmdErrString)
{
    fprintf(stderr, cmdErrString);
    fflush(stderr);
}

/* read_line()
 * -----------
 * Read a line from a FILE*. Will read up until newline or EOF. Note that the
 * newline character will be included in the line as commands being sent to
 * server require a terminating newline character.
 *
 * read: File to read line from
 *
 * Returns: Pointer to line read from file.
 */
char* read_line(FILE* read)
{
    char* result = (char*)calloc(1, sizeof(char));
    int index = 0;

    if (feof(read)) {
        free(result);
        return NULL;
    }
    while (true) {
        char next = fgetc(read);
        // got EOF so exit with whatever has been built up in result up
        // until now
        if (next == EOF) {
            return NULL;
        }

        // add next to result string
        result[index] = next;
        index++;
        result = (char*)realloc(result, (index + 1) * sizeof(char));

        // next was a newline so break here
        if (next == '\n') {
            break;
        }
    }
    // terminate result string
    result[index] = '\0';
    return result;
}

/* move_string_valid()
 * -------------------
 * Check if a move string e.g. "e2e4" is a valid move string or not. This
 * does not consider any game position but rather the validity of the string
 * itself.
 *
 * move: Move string to validate
 *
 * Returns: True if move is a valid move string, false otherwise
 */
bool move_string_valid(char* move)
{
    if ((move == NULL) || (move[0] == '\n') || (!move[0])) {
        return false;
    }
    // -1 to ignore the '\n' character at end of move string
    int moveLen = strlen(move) - 1;

    if (moveLen < MIN_MOVE_STRING_LENGTH || moveLen > MAX_MOVE_STRING_LENGTH) {
        return false;
    }

    // loop to len - 1 since character before null is newline
    for (int i = 0; i < moveLen; i++) {
        if (!isalnum(move[i])) {
            return false;
        }
    }
    return true;
}

/* command_is_valid()
 * ------------------
 * Check if a command is valid based on current game state.
 *
 * command: Full command string e.g. "move e2e4\n"
 * turn: Pointer to flag keeping track of whose turn it is
 * started: Pointer to flag keeping track of whether the game has started
 *
 * Returns: true if the command is valid and can be forwarded to server if
 *          necessary, false otherwise.
 */
bool command_is_valid(GameInfo* info, char* command)
{
    // get tokens of command (only needed for a "move" command)
    char* cmdDup = strdup(command);
    char** args = split_by_char(cmdDup, ' ', 0);

    if (!strcmp(args[0], "newgame\n") || !strcmp(args[0], "print\n")
            || !strcmp(args[0], "hint\n") || !strcmp(args[0], "possible\n")
            || (!strcmp(args[0], "move") && move_string_valid(args[1]))
            || !strcmp(args[0], "resign\n") || !strcmp(args[0], "quit\n")) {
        bool valid = true;
        // user trying to use command other than newgame or quit when the game
        // has not began
        if ((strcmp(args[0], "newgame\n") && strcmp(args[0], "quit\n"))
                && !info->started) {
            command_error(gameNotStartedError);
            free(args);
            free(cmdDup);
            valid = false;
        } else if ((!strcmp(args[0], "move") || !strcmp(args[0], "hint\n")
                           || !strcmp(args[0], "possible\n"))
                && !info->turn) {
            // user trying to get hint or make move when its not their turn
            command_error(turnError);
            free(args);
            free(cmdDup);
            valid = false;
        }
        return valid;
    }
    command_error(invalidCmdError);
    return false;
}

/* translate_command()
 * -------------------
 * Translate a command inputted by user into a command which uqchessserver can
 * understand.
 *
 * info: GameInfo of current game being played
 * userCommand: Command inputted by user to be translated
 *
 * Returns: Pointer to corresponding command to be sent to uqchessserver
 */
char* translate_command(GameInfo* info, char* userCommand)
{
    char* userDup = strdup(userCommand);
    char** args = split_by_char(userDup, ' ', 0);
    char* serverCmd = NULL;

    if (!strcmp(args[0], "newgame\n")) {
        // user wants to start new game so create string
        // "start (opponent) (colour)"
        serverCmd = strdup("start ");
        strcat(serverCmd, info->opponent);
        strcat(serverCmd, " ");
        strcat(serverCmd, info->colour);
    } else if (!strcmp(args[0], "print\n")) {
        serverCmd = strdup("board\n");
    } else if (!strcmp(args[0], "hint\n")) {
        serverCmd = strdup("hint best\n");
    } else if (!strcmp(args[0], "possible\n")) {
        serverCmd = strdup("hint all\n");
    } else if (!strcmp(args[0], "move")) {
        serverCmd = strdup(userCommand);
    } else if (!strcmp(args[0], "resign\n")) {
        serverCmd = strdup(args[0]);
    }
    free(userDup);
    free(args);
    return serverCmd;
}

/* execute_command()
 * -----------------
 * Execute a command from user by sending to server.
 *
 * sockfd: File descriptor of socket connected to server
 * command: Command to execute
 * turn: Pointer to flag keeping track of whose turn it is
 * started: Pointer to flag keeping track of whether the game has begun
 *
 * Returns: true if command was successfully sent to server, false otherwise.
 */
bool execute_command(GameInfo* info, char* command)
{
    // send command string to server
    if (!strcmp(command, "quit\n")) {
        return false;
    }
    char* serverCmd = translate_command(info, command);

    write(info->sockfd, serverCmd, strlen(serverCmd) * sizeof(char));
    char** args = split_by_char(command, ' ', 0);
    // update bool values depending on command
    free(serverCmd);
    free(args);
    return true;
}

/* handle_user_input()
 * -------------------
 * Thread function to handle reading user command input.
 *
 * read: Pointer to Job struct with information necessary for thread to
 *       handle user commands.
 *
 * Returns: NULL pointer
 */
void* handle_user_input(void* read)
{
    GameInfo* info = (GameInfo*)read;
    // immediately start new game upon thread starting
    execute_command(info, "newgame\n");
    // repeatedly read from stdin for user commands
    while (true) {
        char* line = read_line(stdin);

        if (line == NULL) {
            exit(STATUS_OK);
        }

        if (!command_is_valid(info, line)) {
            continue;
        }
        if (!execute_command(info, line)) {
            exit(STATUS_OK);
        }
        free(line);
    }
    return NULL;
}

/* update_info_from_response()
 * ---------------------------
 * Update GameInfo struct based on a server response string.
 *
 * info: GameInfo struct to update
 * response: String of response from uqchessserver
 */
void update_info_from_response(GameInfo* info, char* response)
{
    // Take semaphore
    sem_wait(info->infoLock);
    // update info->turn and info->started accordingly
    if (strstr(response, "started")) {
        if (strstr(response, "white")) {
            info->turn = true;
        } else {
            info->turn = false;
        }
        info->started = true;
    }
    if (!strcmp(response, "ok\n")) {
        info->turn = false;
    }
    if (strstr(response, "error") || strstr(response, "moved")) {
        info->turn = true;
    }
    if (strstr(response, "resign") || strstr(response, "gameover")) {
        info->started = false;
    }
    sem_post(info->infoLock);
}

/* handle_server_response()
 * ------------------------
 * Thread function to handle server responses and print them to stdout.
 *
 * read: Pointer to file descriptor of file to read server responses from
 *
 * Returns: NULL pointer
 */
void* handle_server_response(void* read)
{
    GameInfo* info = (GameInfo*)read;
    FILE* readFile = fdopen(info->sockfd, "r");

    // repeatedly read server response and print to stdout
    while (true) {
        char* response = read_line(readFile);
        if (response == NULL) {
            // no need to free memory so just exit with status 5 (server error)
            fprintf(stderr, "uqchessclient: server has gone away\n");
            exit(SERVER_ERROR);
        }
        // check to see if reponse contained the "startboard" or "endboard"
        // strings that come with response to "board" command
        if (strstr(response, "startboard") || strstr(response, "endboard")) {
            free(response);
            continue;
        }
        printf("%s", response);
        fflush(stdout);
        update_info_from_response(info, response);
        free(response);
    }
    fclose(readFile);
    return NULL;
}

/* init_game()
 * -----------
 * Attempty to initialise a chess game by establishing a connection with
 * uqchessserver.
 *
 * param: Parameters struct containing relevant data given through command line
 *
 * Returns: File descriptor of socket to uqchessserver if connection was
 *          successfull on given port, otherwise PORT_ERROR.
 */
int init_game(Parameters* param)
{
    // setup addrinfo structs to hold server address info and hints to use
    // when searching for server.
    struct addrinfo* ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int status;

    if ((status = getaddrinfo("localhost", param->port, &hints, &ai))) {
        // couldn't find an address
        freeaddrinfo(ai);
        connection_error(param->port);
        return PORT_ERROR;
    }
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if ((sockfd < 0)
            || (connect(sockfd, ai->ai_addr, sizeof(struct addrinfo)))) {
        // connect returned non-zero value (error)
        connection_error(param->port);
        return PORT_ERROR;
    }
    freeaddrinfo(ai);
    return sockfd;
}

/* init_game_info()
 * ----------------
 * Initialise a GameInfo struct from information within a Parameters struct
 * and a given socket file descriptor.
 *
 * param: Parameters struct containing information from command line arguments
 * sockfd: File descriptor of socket connected to uqchessserver
 *
 * Returns: Pointer to GameInfo struct which contains relevant information
 *          about the game being played.
 */
GameInfo* init_game_info(Parameters* param, int sockfd)
{
    GameInfo* result = (GameInfo*)malloc(sizeof(GameInfo));
    sem_t* lock = (sem_t*)malloc(sizeof(sem_t));
    sem_init(lock, 0, 1);

    // initialise result's attributes
    result->infoLock = lock;
    result->started = false;
    result->sockfd = sockfd;
    result->colour = strdup(param->colour);
    result->opponent = strdup(param->opponent);

    // set result->turn depending on whether client is playing as white
    // or black
    if (!strcmp(result->colour, "black\n")) {
        result->turn = false;
    } else {
        result->turn = true;
    }
    return result;
}

int main(int argc, char** argv)
{
    Parameters* param = process_cmdline_args(argc, argv);

    // invalid commandline arguments
    if (param == NULL) {
        fprintf(stderr,
                "Usage: uqchessclient portno [--play computer|human] [--col "
                "black|white]\n");
        exit(USAGE_ERROR);
    }
    // use sigaction to ignore SIGPIPE
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa, 0);
    int sockfd;
    // couldn't establish a connection with uqchessserver
    if ((sockfd = init_game(param)) == PORT_ERROR) {
        exit(PORT_ERROR);
    }

    printf("Welcome to UQChessClient - written by s4834848\n");
    fflush(stdout);
    GameInfo* info = init_game_info(param, sockfd);
    free_parameters(param);
    pthread_t serverTid, stdTid;

    // start two threads to handle user input and server responses
    pthread_create(&serverTid, 0, handle_server_response, info);
    pthread_create(&stdTid, 0, handle_user_input, info);

    // wait for the two threads to finish
    pthread_join(serverTid, NULL);
    pthread_join(stdTid, NULL);
    exit(STATUS_OK);
}
