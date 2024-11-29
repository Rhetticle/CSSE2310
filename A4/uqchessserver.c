#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <ctype.h>
#include <csse2310a4.h>

// Error/Status codes
#define ERROR_USAGE 14
#define ERROR_PORT (-1)
#define ERROR_LISTEN 7
#define ERROR_STOCKFISH_START 11
#define ERROR_STOCKFISH_UNEXPECTED_EXIT 5
#define STATUS_OK 0

// Max and min lengths of a move string
#define MIN_MOVE_STRING_LENGTH 4
#define MAX_MOVE_STRING_LENGTH 5

// Fen string for initial (startpos) position
const char* const initialFen
        = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Constants to represent results of different moves
typedef enum {
    OK,
    CHECK,
    CHECKMATE,
    STALEMATE,
    RESIGN,
    ENGINE,
    COMMAND,
    GAME,
    TURN,
    MOVE
} MoveStatus;

/* GameState
 *
 * Stores information about a current game being played between either two
 * clients or between a client and the computer.
 *
 * lock: Semaphore to use to lock access to struct for thread safety
 * whiteClient: Pointer to Client struct of the client playing as white
 * blackClient: Pointer to Client struct of the client playing as black
 * started: true if game is currently in progress, false if game is over or
 *          hasn't started
 * fenString: FEN string representing most recent game state.
 */
typedef struct {
    sem_t* lock;
    struct Client* whiteClient;
    struct Client* blackClient;
    bool started;
    char* fenString;
} GameState;

/* ClientList
 *
 * Stores a list of Client structs.
 *
 * clients: Array of pointers to Client structs in the list
 * count: Number of Client structs in the list
 * lock: Semaphore to use when accessing list
 */
typedef struct {
    struct Client** clients;
    int count;
    sem_t* lock;
} ClientList;

/* Engine
 *
 * Stores information about a chess engine (Stockfish).
 *
 * toEngine: FILE* to use to send data to engine
 * fromEngine: FILE* to use to read data from engine
 * notifyOnError: List of currently connected clients whom need to be notified
 *                in the case that the engine exits
 * lock: Semaphore to use when accessing engine
 */
typedef struct {
    FILE* toEngine;
    FILE* fromEngine;
    ClientList* notifyOnError;
    sem_t* lock;
} Engine;

/* Client
 *
 * Stores information about a Client to be passed to a thread function.
 *
 * toClient: FILE* to use when writing data to client
 * fromClient: FILE* to use when reading data from client
 * game: Game which client is currently playing (this may be shared between two
 *       client structs if two clients are playing against each other)
 * waitList: List of clients who are currently waiting for a human opponent
 *           with a compatible colour
 * engine: Chess engine for client to use while playing
 * hasPlayed: True if the client has played a game previously, false if not
 * white: True if client wants to play as white, false if black
 * either: True if client wishes to play with either colour (no preferences),
 *         false if client has a preferences.
 * human: True if client wants to play a human, false if wants to play the
 *        computer.
 */
typedef struct Client {
    FILE* toClient;
    FILE* fromClient;
    GameState* game;
    ClientList* waitList;
    Engine* engine;
    bool hasPlayed;
    bool white;
    bool either;
    bool human;
} Client;

/* free_game_state()
 * -----------------
 * Free a GameState struct.
 *
 * state: Pointer to GameState struct to free
 */
void free_game_state(GameState* state)
{
    free(state->fenString);
    sem_destroy(state->lock);
    free(state);
}

/* free_client_list()
 * ------------------
 * Free a list of Client structs.
 *
 * clients: List of clients to free
 * size: number of Client* in clients
 */
void free_client_list(ClientList* list)
{
    for (int i = 0; i < list->count; i++) {
        free(list->clients[i]);
    }
    free(list->lock);
    free(list->clients);
}

/* process_cmdline_args()
 * ----------------------
 * Process command line arguments passed to program to verfiy validity
 * and extract port that user wants server to listen on.
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 *
 * Returns: Pointer to string of port to listen on ("0" if user wishes to use
 *          ephemeral port) OR NULL if the command line arguments are invalid
 */
char* process_cmdline_args(int argc, char** argv)
{
    if (argc == 1) {
        return strdup("0");
    }
    char* port = NULL;

    for (int i = 1; i < argc; i++) {
        if (!(argv[i][0])) {
            // empty string argument
            return NULL;
        }

        if ((!strcmp(argv[i], "--listen") && (i != argc - 1) && (argv[i + 1][0])
                    && (port == NULL))) {
            port = strdup(argv[i + 1]);
            i += 1;
        } else {
            return NULL;
        }
    }
    // args were valid but no port was specified so set port to "0" to be used
    // later when using ephermal port.
    return port;
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
    if ((move == NULL) || (!move[0])) {
        return false;
    }
    int length = strlen(move);

    // move string is too short or too long to be valid
    if ((length < MIN_MOVE_STRING_LENGTH)
            || (length > MAX_MOVE_STRING_LENGTH)) {
        return false;
    }

    for (int i = 0; i < length; i++) {
        if (!isalnum(move[i])) {
            return false;
        }
    }
    return true;
}

/* read_line()
 * -----------
 * Read a line from a FILE* (until '\n' is read).
 *
 * read: FILE* to read line from
 *
 * Returns: Pointer to line read or NULL if error occured whilst trying
 *          to read from FILE*.
 */
char* read_line(FILE* read)
{
    char* result = (char*)malloc(sizeof(char));
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
            if (result != NULL) {
                free(result);
            }
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

/* confirm_stockfish_response()
 * ----------------------------
 * Confirm Stockfish's response to a command to match an expected response.
 *
 * command: Command to issue to Stockfish
 * expected: Expected reponse from Stockfish to command
 * writeFile: FILE* to use to write data to Stockfish
 * readFile: FILE* to use to read data from Stockfish
 *
 * Returns: True if read response from stockfish matched the expected string,
 *          false otherwise.
 */
bool confirm_stockfish_response(
        char* command, char* expected, FILE* writeFile, FILE* readFile)
{
    // send command to stockfish
    if (fputs(command, writeFile) == EOF) {
        return false;
    }
    fflush(writeFile);
    // read response from stockfish
    char* line = read_line(readFile);
    bool flag = false;

    while (line != NULL) {
        // loop until we find the match (we can assume that response is
        // forthcoming)
        if (!strcmp(line, expected)) {
            flag = true;
            free(line);
            break;
        }
        free(line);
        line = read_line(readFile);
    }
    return flag;
}

/* init_stockfish()
 * ----------------
 * Initialise the Stockfish chess engine.
 *
 * writeFile: FILE* to use when sending data to Stockfish
 * readFile: FILE* to use when reading data from Stockfish
 *
 * Returns: Pointer to Engine struct or NULL if Stockfish couldn't be
 *          successfully initialised (isready or uci commands failed)
 */
Engine* init_stockfish(FILE* writeFile, FILE* readFile)
{
    if (feof(readFile) || feof(writeFile)) {
        return NULL;
    }
    // send "isready" and wait for "readyok"
    if (!confirm_stockfish_response(
                "isready\n", "readyok\n", writeFile, readFile)) {
        return NULL;
    }
    // send "uci" and wait for "uciok"
    if (!confirm_stockfish_response("uci\n", "uciok\n", writeFile, readFile)) {
        return NULL;
    }
    // initialise Engine struct and semaphore
    Engine* result = (Engine*)malloc(sizeof(Engine));
    sem_t* stockfishLock = (sem_t*)malloc(sizeof(sem_t));
    sem_init(stockfishLock, 0, 1);

    result->lock = stockfishLock;
    result->toEngine = writeFile;
    result->fromEngine = readFile;

    return result;
}

/* start_stockfish()
 * -----------------
 * Create a child process to run the Stockfish engine.
 *
 * pipeToFish: File descriptors of pipe which will be used to send data to
 *             Stockfish
 * pipeFromFish: File descriptors of pipe which will be used to get data from
 *               Stockfish.
 *
 * Returns: STATUS_OK if Stockfish child was successfully exec()'d otherwise
 *          ERROR_STOCKFISH_START
 */
int start_stockfish(int pipeToFish[2], int pipeFromFish[2])
{
    pipe(pipeToFish);
    pipe(pipeFromFish);

    if (!fork()) {
        // redirect stdout and stdin before starting stockfish
        close(pipeToFish[1]);
        close(pipeFromFish[0]);
        dup2(pipeFromFish[1], STDOUT_FILENO);
        dup2(pipeToFish[0], STDIN_FILENO);
        close(pipeFromFish[1]);
        close(pipeToFish[0]);

        execlp("stockfish", "stockfish", NULL);
    }
    // in parent so closed read end of pipe to Stockfish and close
    // write end of pipe from Stockfish
    close(pipeToFish[0]);
    close(pipeFromFish[1]);
    return STATUS_OK;
}

/* init_socket()
 * -------------
 * Initialise a socket to list on a given port.
 *
 * port: Port to listen on ("0" if to use ephemeral port)
 * sockfd: Pointer to variable to store socket file descriptor
 *
 * Returns: Port number which socket is listening on if successfull, otherwise
 *          ERROR_PORT
 */
int init_socket(char* port, int* sockfd)
{
    // setup addrinfo struct with hints and address family
    struct addrinfo* ai = 0;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status;

    if ((status = getaddrinfo("localhost", port, &hints, &ai))) {
        // couldn't get address information
        freeaddrinfo(ai);
        return ERROR_PORT;
    }
    *sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(*sockfd, ai->ai_addr, sizeof(struct addrinfo))) {
        // couldn't bind socket
        freeaddrinfo(ai);
        return ERROR_PORT;
    }
    // get information about who connected
    struct sockaddr_in ad;
    memset(&ad, 0, sizeof(struct sockaddr_in));
    socklen_t len = sizeof(struct sockaddr_in);
    getsockname(*sockfd, (struct sockaddr*)&ad, &len);

    if (listen(*sockfd, 0)) {
        freeaddrinfo(ai);
        return ERROR_PORT;
    }
    return ntohs(ad.sin_port);
}

/* send_to_client()
 * ----------------
 * Send message to a client connected to serve.
 *
 * message: Message to send to client
 * client: Client to send message to
 */
void send_to_client(char* message, Client* client)
{
    fputs(message, client->toClient);
    fflush(client->toClient);
}

/* get_opponent()
 * --------------
 * Get the opposing client of a given client.
 *
 * client: Client to get opponent of
 *
 * Returns: Pointer to client which client is versing in their game
 */
Client* get_opponent(Client* client)
{
    Client* opponent;

    if (client->white) {
        opponent = client->game->blackClient;
    } else {
        opponent = client->game->whiteClient;
    }
    return opponent;
}

/* send_error()
 * ------------
 * Send an error string to a client.
 *
 * client: Client to send error message to
 * error: MoveStatus representing error which occured
 */
void send_error(Client* client, MoveStatus error)
{
    char* errString = NULL;

    switch (error) {
    case ENGINE:
        errString = "error engine\n";
        break;
    case COMMAND:
        errString = "error command\n";
        break;
    case GAME:
        errString = "error game\n";
        break;
    case TURN:
        errString = "error turn\n";
        break;
    default:
        errString = "error move\n";
        break;
    }
    send_to_client(errString, client);
}

/* handle_stockfish_exit()
 * -----------------------
 * Clean up after the chess engine (Stockfish) exits unexpectedly. The child
 * process will be reaped and all connected clients will be notified with
 * "error engine" before the program exits.
 *
 * Engine: Pointer to engine which failed (Stockfish)
 */
void handle_stockfish_exit(Engine* engine)
{
    // reap stockfish process
    wait(NULL);
    sem_wait(engine->notifyOnError->lock);
    for (int i = 0; i < engine->notifyOnError->count; i++) {
        // send "error engine" to all connected clients
        send_error(engine->notifyOnError->clients[i], ENGINE);
    }
    sem_post(engine->notifyOnError->lock);
    fprintf(stderr, "uqchessserver: chess engine exited unexpectedly\n");
    exit(ERROR_STOCKFISH_UNEXPECTED_EXIT);
}

/* remove_from_list()
 * ------------------
 * Remove an entry from a ClientList at position index.
 *
 * list: List to remove entry from.
 * index: Index of list->clients to remove entry from
 */
void remove_from_list(ClientList* list, Client* remove)
{
    Client** newList = NULL;
    int added = 0;

    for (int i = 0; i < list->count; i++) {
        if (list->clients[i] == remove) {
            // same pointer as the one we wish to remove from list so don't
            // add to newList
            continue;
        }
        newList = (Client**)realloc(newList, (added + 1) * sizeof(Client*));
        newList[added] = list->clients[i];
        added++;
    }
    list->clients = newList;
    list->count = added;
}

/* flush_stockfish()
 * -----------------
 * Flush data to stockfish. Function will check if fflush() fails and
 * call handle_stockfish_exit() accordingly.
 *
 * engine: Pointer to engine to flush data to
 */
void flush_stockfish(Engine* engine)
{
    if (fflush(engine->toEngine) == EOF) {
        handle_stockfish_exit(engine);
    }
}

/* send_to_stockfish()
 * -------------------
 * Send a constant string to the Stockfish engine.
 *
 * engine: Engine to send command to
 */
void send_to_stockfish(char* command, Engine* engine)
{
    fputs(command, engine->toEngine);
    flush_stockfish(engine);
}

/* send_started()
 * --------------
 * Send a "started" message to a client (either "started white\n" or
 * "started black\n").
 *
 * client: Client to send started message to
 */
void send_started(Client* client)
{
    // set hasPlayed attribute of client
    client->hasPlayed = true;
    if (client->white) {
        send_to_client("started white\n", client);
    } else {
        send_to_client("started black\n", client);
    }
}

/* send_ok()
 * ---------
 * Send an "ok" message to a client.
 *
 * client: Client to send "ok" message to
 */
void send_ok(Client* client)
{
    send_to_client("ok\n", client);
}

/* is_game_over()
 * -------------
 * Determine is a game is over for a given MoveStatus.
 *
 * status: Resulting MoveStatus of most recent move that was played
 *
 * Returns: True if stauts is CHECKMATE, STALEMATE or RESIGN, false otherwise
 */
bool is_game_over(MoveStatus status)
{
    if ((status == CHECKMATE) || (status == STALEMATE) || (status == RESIGN)) {
        return true;
    }
    return false;
}

/* send_gameover()
 * ---------------
 * Send a "gameover" message to a client with a given reason for the game
 * ending.
 *
 * client: Client to send "gameover" message to
 * reason: Reason for game being over (e.g. CHECKMATE, STALEMATE or RESIGN)
 */
void send_gameover(Client* client, MoveStatus reason)
{
    GameState* state = client->game;
    char turn = next_player_from_fen_string(state->fenString);
    char* winner;

    if (turn == 'w') {
        winner = "black";
    } else {
        winner = "white";
    }
    // send gameover message for corresponding gameover reason
    if (reason == CHECKMATE) {
        fprintf(client->toClient, "gameover checkmate %s\n", winner);
    } else if (reason == STALEMATE) {
        send_to_client("gameover stalemate\n", client);
    } else if (reason == RESIGN) {
        fprintf(client->toClient, "gameover resignation %s\n", winner);
    }
    fflush(client->toClient);
}

/* notify_client()
 * ---------------
 * Notify client of an event (gameover or check). If client is versing
 * another client, their opponent will also get notified appropriately.
 *
 * client: Origin client to be notified.
 * status: MoveStatus of most recent move which was played
 */
void notify_client(Client* client, MoveStatus status)
{
    if (!client->hasPlayed) {
        return;
    }
    Client* opponent = get_opponent(client);
    // game hasn't started so no need to notify
    if (!client->game->started) {
        return;
    }
    if (is_game_over(status)) {
        send_gameover(client, status);

        if (client->human && (get_opponent(client) != NULL)) {
            // send game over to opponent if they're human
            send_gameover(opponent, status);
        }
    } else if (status == CHECK) {
        send_to_client("check\n", client);

        if (client->human) {
            // send check to opponent if they're human
            send_to_client("check\n", opponent);
        }
    }
}

/* clean_up_client()
 * -----------------
 * Handle a client which has recently disconnected from the server. Their
 * opponent will be notified of a win by resignation. If the client never
 * played a game they will also be removed from the wait list.
 *
 * leaving: Client which disconnected from the server.
 */
void clean_up_client(Client* leaving)
{
    notify_client(leaving, RESIGN);
    // remove client from connected client list
    sem_wait(leaving->engine->notifyOnError->lock);
    remove_from_list(leaving->engine->notifyOnError, leaving);
    sem_post(leaving->engine->notifyOnError->lock);

    if (leaving->human) {
        // client was playing a human so we will check if their opponent
        // is NULL meaning they were not in a game when they left (so on the
        // wait list)
        sem_wait(leaving->waitList->lock);
        Client* opponent = get_opponent(leaving);
        if (opponent == NULL) {
            // remove from the wait list
            remove_from_list(leaving->waitList, leaving);
        }
        sem_post(leaving->waitList->lock);
    }
    if (leaving->hasPlayed) {
        if (!leaving->human || (get_opponent(leaving) == NULL)) {
            // the exiting client was playing against the computer OR was not
            // matched up against a human opponent so we can safely free it's
            // GameState as no other clients rely on it
            free_game_state(leaving->game);
        } else if (get_opponent(leaving) != NULL) {
            if (leaving->white) {
                leaving->game->whiteClient = NULL;
            } else {
                leaving->game->blackClient = NULL;
            }
        }
    }
    pthread_exit(STATUS_OK);
}

/* get_client_info()
 * -----------------
 * Get information about a newly connected client (desired colour and opponent
 * type).
 *
 * store: Client struct to store information in
 */
bool get_client_info(Client* store)
{
    store->human = false;
    store->white = false;
    store->either = false;
    store->hasPlayed = false;
    char* line = read_line(store->fromClient);

    if (line == NULL) {
        // client disconnected
        clean_up_client(store);
    }
    char** parts = split_by_char(line, ' ', 0);

    if (strcmp(parts[0], "start")) {
        if (!strcmp(parts[0], "board\n") || !strcmp(parts[0], "move")
                || !strcmp(parts[0], "hint") || !strcmp(parts[0], "resign\n")) {
            send_error(store, GAME);
        } else {
            send_error(store, COMMAND);
        }
        free(parts);
        free(line);
        return false;
    }
    // set Client struct human and white attributes according to client
    // message
    if (!strcmp(parts[1], "human")) {
        store->human = true;
    }
    if (!strcmp(parts[2], "white\n")) {
        store->white = true;
    } else if (!strcmp(parts[2], "black\n")) {
        store->white = false;
    } else if (!strcmp(parts[2], "either\n")) {
        if (store->human) {
            store->either = true;
        } else {
            store->white = true;
        }
    }
    free(parts);
    free(line);
    return true;
}

/* clients_are_compatible()
 * ------------------------
 * Check if two clients are compatible based on their colour preferences
 * (white, black or either).
 *
 * waiting: Client from the wait list who is waiting for a compatible opponent
 * looking: Newly connected client looking for an opponent
 *
 * Returns: True if waiting and looking clients are compatible based on their
 *          colour preferences, otherwise false.
 */
bool clients_are_compatible(Client* waiting, Client* looking)
{
    sem_wait(waiting->game->lock);
    bool compatible = false;

    if (waiting->either && looking->either) {
        waiting->white = true;
        looking->white = false;
        waiting->game->blackClient = looking;
        compatible = true;
    }
    if (waiting->either && !looking->white) {
        waiting->game->blackClient = looking;
        waiting->white = true;
        compatible = true;
    }
    if (waiting->either && looking->white) {
        waiting->game->whiteClient = looking;
        waiting->game->blackClient = waiting;
        compatible = true;
    }
    if (looking->either && !waiting->white) {
        looking->white = true;
        waiting->game->blackClient = waiting;
        waiting->game->whiteClient = looking;
        compatible = true;
    }
    if (looking->either && waiting->white) {
        looking->white = false;
        waiting->game->whiteClient = waiting;
        waiting->game->blackClient = looking;
        compatible = true;
    }
    if (looking->white == !waiting->white) {
        if (waiting->white) {
            waiting->game->whiteClient = waiting;
            waiting->game->blackClient = looking;
        } else {
            waiting->game->blackClient = waiting;
            waiting->game->whiteClient = looking;
        }
        compatible = true;
    }
    sem_post(waiting->game->lock);
    return compatible;
}

/* find_opponent()
 * ---------------
 * Attempt to find looking a colour compatible opponent from waitList. If no
 * such client can be found then looking client will be added to waitList.
 *
 * waitList: List of clients currently waiting to play a human vs human game
 * looking: Client which is not in the waitList who is looking for an opponent
 *
 * Returns: True if a colour compatible opponent for looking is found in
 *          waitList, false otherwise.
 */
bool find_opponent(ClientList* waitList, Client* looking)
{
    for (int i = 0; i < waitList->count; i++) {
        sem_wait(waitList->lock);
        Client* waiting = waitList->clients[i];

        if ((waiting->white == !looking->white) || (waiting->either)
                || (looking->either)) {
            // waiting client and new client have compatible colours
            // so update the GameState client attributes

            if (!clients_are_compatible(waiting, looking)) {
                continue;
            }
            // update the looking client with the same game as the waiting
            // client and free the looking client's temporary GameState
            free_game_state(looking->game);
            looking->game = waiting->game;
            waiting->game->started = true;
            // remove the waiting client from the wait list
            remove_from_list(waitList, waiting);
            // send "started" messages to clients
            send_started(waiting);
            send_started(looking);
            sem_post(waitList->lock);
            return true;
        }
        sem_post(waitList->lock);
    }
    return false;
}

/* add_to_waitlist()
 * -----------------
 * Add a client to the wait list of clients looking for opponents.
 *
 * waitList: List of clients who are waiting to find an opponent
 * add: Client to add to waitList
 */
void add_to_waitlist(ClientList* waitList, Client* add)
{
    // take semaphore and update waitList with new client
    sem_wait(waitList->lock);
    waitList->clients = (Client**)realloc(
            waitList->clients, (waitList->count + 1) * sizeof(Client*));
    waitList->clients[waitList->count] = add;
    waitList->count++;
    sem_post(waitList->lock);
}

/* set_position()
 * --------------
 * Set the position of the Stockfish engine to the position contained in
 * fenString.
 *
 * fenString: FEN string of position to set Stockfish to
 * engine: Pointer to engine to use
 */
void set_position(char* fenString, Engine* engine)
{
    send_to_stockfish("ucinewgame\n", engine);
    confirm_stockfish_response(
            "isready\n", "readyok\n", engine->toEngine, engine->fromEngine);
    fprintf(engine->toEngine, "position fen %s\n", fenString);
    flush_stockfish(engine);
}

/* get_best_move()
 * ---------------
 * Get the best move for a current position.
 *
 * fenString: FEN string of position to get best move for
 * engine: Engine to use to evaluate best move
 *
 * Returns: String representation of best move e.g. "g1f3"
 */
char* get_best_move(char* fenString, Engine* engine)
{
    set_position(fenString, engine);
    // send command to stockfish to analyse position and read response
    send_to_stockfish("go movetime 500 depth 15\n", engine);
    ChessMoves* best = read_stockfish_bestmove_output(engine->fromEngine);
    char* bestMove = strdup(best->moves[0]);
    free_chess_moves(best);
    return bestMove;
}

/* is_clients_turn()
 * -----------------
 * Check if it is client's turn base on FEN string of current state of game.
 *
 * client: Client to check if it is their turn or not
 *
 * Returns: True if the FEN string indicates that it is client's turn, false
 *          otherwise.
 */
bool is_clients_turn(Client* client)
{
    char nextTurn = next_player_from_fen_string(client->game->fenString);
    bool result = false;

    if ((nextTurn == 'w') && client->white) {
        result = true;
    } else if ((nextTurn == 'b') && !client->white) {
        result = true;
    }
    return result;
}

/* get_possible_moves()
 * --------------------
 * Get all possible moves for a given position.
 *
 * fenString: FEN string of position to get all possible moves of
 * engine: Engine to use to get all possible moves
 *
 * Returns: Pointer to ChessMoves struct whos "moves" attribute contains
 *          all the possible moves in their string representation
 */
ChessMoves* get_possible_moves(char* fenString, Engine* engine)
{
    set_position(fenString, engine);
    send_to_stockfish("go perft 1\n", engine);
    ChessMoves* possible = read_stockfish_go_perft_1_output(engine->fromEngine);
    return possible;
}

/* send_hints()
 * ------------
 * Send response to a "hint" command from client (either "hint all" or
 * "hint best".
 *
 * client: Client to send hints to
 * option: Either "all" if client wants hints of all possible moves or "best"
 *         if client wants to know the best move
 */
void send_hints(Client* client, char* option)
{
    sem_wait(client->engine->lock);
    sem_wait(client->game->lock);
    // clients game has not started so ignore request
    if (!client->game->started) {
        sem_post(client->game->lock);
        sem_post(client->engine->lock);
        send_error(client, GAME);
        return;
    }
    // not clients turn so ignore request
    if (!is_clients_turn(client)) {
        sem_post(client->game->lock);
        sem_post(client->engine->lock);
        send_error(client, TURN);
        return;
    }
    if (!strcmp(option, "best\n")) {
        // client wants best move so ask stockfish and send result
        char* best = get_best_move(client->game->fenString, client->engine);
        fprintf(client->toClient, "moves %s\n", best);
        fflush(client->toClient);
        free(best);
    } else if (!strcmp(option, "all\n")) {
        // client wants all possible moves for current position so ask
        // Stockfish and send all elements of possible->moves
        ChessMoves* possible
                = get_possible_moves(client->game->fenString, client->engine);
        send_to_client("moves", client);
        for (int i = 0; i < possible->numMoves; i++) {
            send_to_client(" ", client);
            send_to_client(possible->moves[i], client);
        }
        send_to_client("\n", client);
        free_chess_moves(possible);
    } else {
        send_error(client, COMMAND);
    }
    sem_post(client->game->lock);
    sem_post(client->engine->lock);
}

/* init_new_game()
 * ---------------
 * Initialise a new game empty to be assigned to client.
 *
 * client: Client to assign new GameState struct to (this may end up also
 *         getting assigned to another client in future if client was to
 *         play against another human
 *
 * Returns: Pointer to initialised GameState struct.
 */
GameState* init_new_game(Client* client)
{
    // Allocate for GameState and initialise the semaphore
    GameState* result = (GameState*)malloc(sizeof(GameState));
    result->lock = (sem_t*)malloc(sizeof(sem_t));
    sem_init(result->lock, 0, 1);

    // setup game attributes based on client attributes (Note that blackClient
    // or whiteClient will be reassigned from NULL to another client if this
    // client finds an opponent in future
    if (client->white || client->either) {
        result->whiteClient = client;
        result->blackClient = NULL;
    } else {
        result->blackClient = client;
        result->whiteClient = NULL;
    }
    if (client->human) {
        result->started = false;
    } else {
        result->started = true;
        send_started(client);
    }
    // setup initial fen string
    result->fenString = strdup(initialFen);
    return result;
}

/* get_stockfish_state()
 * ---------------------
 * Get current state of game from Stockfish (used to get board layout of game)
 * (Note that the current position of Stockfish must be configured before
 * calling this function to ensure correct position is read).
 *
 * engine: Engine to get state from
 *
 * Returns: Pointer to StockfishGameState struct which holds information about
 *          current game state
 */
StockfishGameState* get_stockfish_state(Engine* engine)
{
    send_to_stockfish("d\n", engine);
    StockfishGameState* state = read_stockfish_d_output(engine->fromEngine);
    return state;
}

/* send_board()
 * ------------
 * Send current board state to client.
 *
 * engine: Engine to use to get board state
 * client: Client to send board state to
 */
void send_board(Engine* engine, Client* client)
{
    // client's game hasn't started to ignore request
    if (!client->hasPlayed) {
        send_error(client, GAME);
        return;
    }
    // set position of Stockfish and send state->boardString to client
    StockfishGameState* state = get_stockfish_state(engine);
    send_to_client("startboard\n", client);
    send_to_client(state->boardString, client);
    send_to_client("endboard\n", client);
    free_stockfish_game_state(state);
}

/* get_fen_string()
 * ----------------
 * Get FEN string of a current position from Stockfish (Used to get new
 * FEN string after a move has been made by client/computer)
 *
 * engine: Engine to use to get FEN string from
 *
 * Returns: FEN string of current Stockfish position
 */
char* get_fen_string(Engine* engine)
{
    StockfishGameState* state = get_stockfish_state(engine);
    char* fen = strdup(state->fenString);
    free_stockfish_game_state(state);
    return fen;
}

/* send_move_to_stockfish()
 * ------------------------
 * Send a move to Stockfish for curren position and make sure it was accepted
 * as valid by Stockfish.
 *
 * move: String representation of move to make
 * game: Current state of game (before move is played)
 * engine: Engine to use to make move
 *
 * Returns: OK if move was valid, TURN if it's not players turn OR MOVE if the
 *          move is invalid and not accepted by Stockfish
 */
StockfishGameState* send_move_to_stockfish(
        char* move, GameState* game, Engine* engine)
{
    // send move to Stockfish get updated position
    send_to_stockfish("ucinewgame\n", engine);
    confirm_stockfish_response(
            "isready\n", "readyok\n", engine->toEngine, engine->fromEngine);
    fprintf(engine->toEngine, "position fen %s moves %s\n", game->fenString,
            move);
    flush_stockfish(engine);
    StockfishGameState* result = get_stockfish_state(engine);

    // FEN string did not change meaning move was invalid
    if (!strcmp(game->fenString, result->fenString)) {
        result = NULL;
    } else {
        // update game with new FEN string
        game->fenString = result->fenString;
    }
    return result;
}

/* analyse_position()
 * ------------------
 * Get engine to analyse current position for a check, checkmate or stalemate.
 *
 * game: Game to get engine to analyse.
 * engine: Engine to use to analyse position
 *
 * Returns: OK if current position not has a check, checkmate or stalemate,
 *          otherwise CHECK, CHECKMATE or STALEMATE if current position
 *          contains a check, checkmate or stalemate respectively.
 */
MoveStatus analyse_position(StockfishGameState* game, Engine* engine)
{
    ChessMoves* lastMoves;
    MoveStatus result = OK;
    // Get a ChessMoves struct for current position to use to check for
    // check, checkmate or stalemate
    send_to_stockfish("go perft 1\n", engine);
    lastMoves = read_stockfish_go_perft_1_output(engine->fromEngine);

    if (game->checkers != NULL) {
        if (lastMoves->moves == NULL) {
            // there was a check and there are no possible moves (checkmate)
            result = CHECKMATE;
        } else {
            // there was a check and there are possible moves (check)
            result = CHECK;
        }
    } else if ((game->checkers == NULL) && (lastMoves->moves == NULL)) {
        // there was no check but there a no possible moves (stalemate)
        result = STALEMATE;
    }
    free_chess_moves(lastMoves);
    return result;
}

/* client_made_valid_move()
 * ------------------------
 * Check if client made a valid move based on the status of their move.
 *
 * status: MoveStatus of client's move
 *
 * Returns: True if status is OK or CHECK, false otherwise (all other
 *          MoveStatus's indicate something went wrong or game is over)
 */
bool client_made_valid_move(MoveStatus status)
{
    if ((status == OK) || (status == CHECK)) {
        return true;
    }
    return false;
}

/* client_move_valid()
 * -------------------
 * Check if a client trying to make a given move is valid.
 *
 * client: Client who is making the move
 * move: Move string of move which client wishes to play
 *
 * Returns: MoveStatus explaining whether move is valid or not, OK if move is
 *          valid otherwise the reason (GAME, TURN or MOVE)
 */
MoveStatus client_move_valid(Client* client, char* move)
{
    if (!client->game->started) {
        return GAME;
    }
    if (!is_clients_turn(client)) {
        return TURN;
    }
    if (!move_string_valid(move)) {
        return COMMAND;
    }
    return OK;
}

/* client_move()
 * -------------
 * Let a client make their move and notify opponent client of move if
 * opponent is human.
 *
 * client: Client which is making move
 * move: Move that client wishes to play
 *
 * Returns: MoveStatus of client's move
 */
MoveStatus client_move(Client* client, char* move)
{
    sem_wait(client->engine->lock);
    sem_wait(client->game->lock);
    MoveStatus moveValid = client_move_valid(client, move);

    // move which client wants to make is invalid so release semaphores and
    // send and error
    if (moveValid != OK) {
        sem_post(client->game->lock);
        sem_post(client->engine->lock);
        send_error(client, moveValid);
        return moveValid;
    }
    StockfishGameState* state
            = send_move_to_stockfish(move, client->game, client->engine);

    // move wasn't valid according to Stockfish so send an error just to client
    // which is trying to make a move
    if (state == NULL) {
        send_error(client, MOVE);
        sem_post(client->game->lock);
        sem_post(client->engine->lock);
        return MOVE;
    }
    send_ok(client);
    if (client->human) {
        // if client's opponent is human let opponent know the move which was
        // played
        Client* opp = get_opponent(client);
        fprintf(opp->toClient, "moved %s\n", move);
        fflush(opp->toClient);
    }
    // Analyse position for gameover, check or stalemate
    MoveStatus clientMoveStatus = analyse_position(state, client->engine);
    notify_client(client, clientMoveStatus);

    // change game->started if the move just made resulted in the game ending
    if (is_game_over(clientMoveStatus)) {
        client->game->started = false;
    }
    sem_post(client->game->lock);
    sem_post(client->engine->lock);
    return clientMoveStatus;
}

/* computer_move()
 * ---------------
 * Let computer make move against client.
 *
 * opponent: Computer's opponent client
 *
 * Returns: MoveStatus of computers move
 */
MoveStatus computer_move(Client* opponent)
{
    sem_wait(opponent->engine->lock);
    sem_wait(opponent->game->lock);
    // get best move from stockfish and play it
    char* best = get_best_move(opponent->game->fenString, opponent->engine);
    StockfishGameState* state
            = send_move_to_stockfish(best, opponent->game, opponent->engine);
    fprintf(opponent->toClient, "moved %s\n", best);
    fflush(opponent->toClient);
    free(best);

    // analyse position for checks, checkmate or stalemate
    MoveStatus status = analyse_position(state, opponent->engine);
    notify_client(opponent, status);
    // change game->started if move made resulted in the game ending
    if (is_game_over(status)) {
        opponent->game->started = false;
    }
    sem_post(opponent->game->lock);
    sem_post(opponent->engine->lock);
    return status;
}

/* match_up_client()
 * -----------------
 * Attempt to match up a client with an opponent based on client preferences.
 * If looking is wanting to verse the computer it is given a new GameState and
 * can begin playing. If looking wants a human opponent we will try to find one
 * from the waitList and if that fails they will be given a new GameState and
 * placed on the waitList.
 *
 * looking: Client which is looking for an opponent.
 */
void match_up_client(Client* looking)
{
    if (looking->human) {
        ClientList* waitList = looking->waitList;
        // we will give the looking client a temporary GameState, note that if
        // find_opponent() does indeed find an opponent from the wait list then
        // looking->game will point to the waiting client's GameState and this
        // temporary state will be freed.
        looking->game = init_new_game(looking);
        // if client wants to play another human we will look for a match
        // from the waitList
        if (!find_opponent(waitList, looking)) {
            // couldn't find a compatible opponent for client so add them to
            // the waitList
            add_to_waitlist(waitList, looking);
        }
    } else {
        // client wants to play computer to just give them a new GameState. If
        // client has previously played we need to free the old GameState.
        if (looking->hasPlayed) {
            free_game_state(looking->game);
        }
        looking->game = init_new_game(looking);

        if (!looking->white) {
            // set_position(looking->game->fenString, looking->engine);
            computer_move(looking);
        }
    }
}

/* handle_resign()
 * ---------------
 * Handle a client who recently resigned from their game. Client's opponent
 * will be notified (if human) and client->game->started will be set to false.
 *
 * client: Client who is resigning.
 */
void handle_resign(Client* client)
{
    // Client isn't in a game so send an error
    sem_wait(client->game->lock);
    if (!client->game->started) {
        send_error(client, GAME);
    } else {
        // notify opponent and change game->started
        notify_client(client, RESIGN);
        client->game->started = false;
    }
    sem_post(client->game->lock);
}

/* handle_client_input()
 * ---------------------
 * Handle input commands from a client.
 *
 * client: Client to handle commands of
 */
void handle_client_input(Client* client)
{
    char* clientInput = read_line(client->fromClient);
    // client has disconnected so end thread and notify opponent of resignation
    if (clientInput == NULL) {
        clean_up_client(client);
    }
    Engine* engine = client->engine;
    char* inputDup = strdup(clientInput);
    char** parts = split_by_char(inputDup, ' ', 0);

    if (!strcmp(clientInput, "board\n")) {
        if (client->game->started) {
            set_position(client->game->fenString, engine);
        }
        send_board(engine, client);
    } else if (!strcmp(parts[0], "move")) {
        parts[1][strlen(parts[1]) - 1] = '\0';
        MoveStatus clientMove = client_move(client, parts[1]);
        if (is_game_over(clientMove)) {
            free(parts);
            free(inputDup);
            free(clientInput);
            return;
        }
        // if client is playing computer and just made a valid move we let the
        // computer make their move
        if (!client->human && client_made_valid_move(clientMove)) {
            if (is_game_over(computer_move(client))) {
                free(parts);
                free(inputDup);
                free(clientInput);
                return;
            }
        }
    } else if (!strcmp(parts[0], "hint")) {
        send_hints(client, parts[1]);
    } else if (!strcmp(clientInput, "resign\n")) {
        handle_resign(client);
    } else if (!strcmp(parts[0], "start")) {
        // client wants newgame to attempt to match them up and set
        // Stockfish to the initial position
        match_up_client(client);
    } else {
        send_error(client, COMMAND);
    }
    free(parts);
    free(inputDup);
    free(clientInput);
}

/* handle_connection()
 * -------------------
 * Thread function to handle a newly connected client. Thread will continuously
 * loop handling client commands until client disconnects and thread exits.
 *
 * inputClient: Pointer to client which just connected.
 *
 * Returns: NULL
 */
void* handle_connection(void* inputClient)
{
    Client* client = (Client*)inputClient;
    // get info about client (colour and opponent type)
    bool gotClientInfo = get_client_info(client);

    // wait here until we get a "start" message from client
    while (!gotClientInfo) {
        gotClientInfo = get_client_info(client);
    }
    match_up_client(client);

    while (true) {
        handle_client_input(client);
    }
    return NULL;
}

/* init_sigaction()
 * ----------------
 * Initialise signal handler (SIG_IGN) for SIGPIPE using sigaction
 */
void init_sigaction(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa, 0);
}

/* init_list()
 * -----------
 * Initialise an empty ClientList struct. Will initialise semaphore
 * but will not allocate space for Client* array.
 *
 * Returns: Pointer to empty ClientList.
 */
ClientList* init_list(void)
{
    ClientList* list = (ClientList*)malloc(sizeof(ClientList));
    list->lock = (sem_t*)malloc(sizeof(sem_t));
    list->clients = NULL;
    list->count = 0;
    sem_init(list->lock, 0, 1);
    return list;
}

/* client_loop()
 * -------------
 * Loop to continually accept and initialise new clients connecting to the
 * server. This function should never return.
 *
 * sockfd: File descriptor of socket to accept connections on
 * Engine: Engine struct to use for all chess computations
 */
void client_loop(int sockfd, Engine* engine)
{
    // Initialise necessary server data structures
    ClientList* connected = init_list();
    connected->clients = (Client**)malloc(sizeof(Client*));
    ClientList* waitList = init_list();
    // update engine->notifyOnError to point at array of connected clients
    engine->notifyOnError = connected;

    while (true) {
        int connfd;
        pthread_t tid;

        if ((connfd = accept(sockfd, 0, 0))) {
            // new client was accepted so initialise their corresponding
            // Client struct to be passed to thread function
            // handle_connnection().
            sem_wait(connected->lock);
            int threadCount = connected->count;
            connected->clients = (Client**)realloc(
                    connected->clients, (threadCount + 1) * sizeof(Client*));
            connected->clients[threadCount] = (Client*)malloc(sizeof(Client));
            connected->clients[threadCount]->toClient = fdopen(connfd, "r+");
            connected->clients[threadCount]->fromClient
                    = fdopen(dup(connfd), "r+");
            connected->clients[threadCount]->waitList = waitList;
            connected->clients[threadCount]->engine = engine;
            connected->count++;
            sem_post(connected->lock);
            pthread_create(&tid, 0, handle_connection,
                    connected->clients[threadCount]);
            pthread_detach(tid);
        }
    }
}

int main(int argc, char** argv)
{
    char* port = process_cmdline_args(argc, argv);

    // invalid port
    if (port == NULL) {
        fprintf(stderr, "Usage: ./uqchessserver [--listen portnum]\n");
        free(port);
        exit(ERROR_USAGE);
    }
    int sockfd;
    int portListen = init_socket(port, &sockfd);
    init_sigaction();

    // couldn't listen on given port
    if (portListen == ERROR_PORT) {
        fprintf(stderr, "uqchessserver: can't listen on port \"%s\"\n", port);
        free(port);
        exit(ERROR_LISTEN);
    }
    int pipeToFish[2], pipeFromFish[2];

    // fork and redirect stockfish stdout and stdin
    start_stockfish(pipeToFish, pipeFromFish);

    // Create an engine struct with writeFile and readFile going to and from
    // stockfish respectively
    FILE* writeFile = fdopen(pipeToFish[1], "w");
    FILE* readFile = fdopen(pipeFromFish[0], "r");
    Engine* engine = init_stockfish(writeFile, readFile);

    // couldn't establish communication with stockfish
    if (engine == NULL) {
        fclose(readFile);
        fclose(writeFile);
        fprintf(stderr,
                "uqchessserver: unable to start communication with chess "
                "engine\n");
        free(port);
        exit(ERROR_STOCKFISH_START);
    }
    fprintf(stderr, "%u\n", portListen);
    fflush(stderr);
    // call client_loop() which should never return
    client_loop(sockfd, engine);
    exit(STATUS_OK);
}
