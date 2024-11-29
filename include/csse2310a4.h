/*
 * csse2310a4.h
 */

#ifndef CSSE2310A4_H
#define CSSE2310A4_H

#include <stdio.h>
#include <stdbool.h>

char** split_by_char(char* str, char split, unsigned int maxFields);

typedef struct {
    char* boardString;
    char* fenString;
    char* checkers;
    bool whiteToPlay;
} StockfishGameState;

StockfishGameState* read_stockfish_d_output(FILE* stream);
void free_stockfish_game_state(StockfishGameState* state);

typedef struct {
    int numMoves;
    char** moves;
} ChessMoves;

ChessMoves* read_stockfish_go_perft_1_output(FILE* stream);
ChessMoves* read_stockfish_bestmove_output(FILE* stream);
void free_chess_moves(ChessMoves* moves);

char next_player_from_fen_string(const char* fen);

typedef struct {
    char* name;
    char* value;
} HttpHeader;

void free_header(HttpHeader* header);
void free_array_of_headers(HttpHeader** headers);

int get_HTTP_request(FILE* f, char** method, char** address, 
        HttpHeader*** headers, unsigned char** body, unsigned long* bodySize);

unsigned char* construct_HTTP_response(int status, const char* statusExplanation, 
	HttpHeader** headers, const unsigned char* body, 
        unsigned long bodySize, unsigned long* len);

int get_HTTP_response(FILE* f, int* httpStatus, char** statusExplain, 
        HttpHeader*** headers, unsigned char** body, unsigned long* bodySize);

#endif
