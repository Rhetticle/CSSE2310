#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stddef.h>

// Base Character set sizes
#define NUMERIC_SET_SIZE 10
#define ALPHABET_SET_SIZE 26
#define ASCII_OTHER_SET_SIZE 32

// Password strength entropy ratings
#define ENTROPY_WEAK 35
#define ENTROPY_STRONG 60
#define ENTROPY_VERY_STRONG 120

// Exit codes
#define EXIT_INVALID_USAGE 18
#define EXIT_FILE_ERROR 7
#define EXIT_NO_STRONG_PASSWORD 8

// Factor to multiply float by when using floor()
#define ROUND_FACTOR 10

// Maximum and minimum number of digits that can be appended when using
// --digit-append option
#define DIG_APPEND_MAX 6
#define DIG_APPEND_MIN 1

// Bases used when calculating number of guesses to add when checking
// for leet speak i.e. count = 2^(a)3^(b) - 1
#define LEET_COUNT_FIRST_BASE 2
#define LEET_COUNT_SECOND_BASE 3

// Base used when calculating number of guesses to add when checking
// digit append i.e. count = 10^1+...+10^n
#define DIG_APPEND_COUNT_BASE 10

// Number of different search options (excluding EXACT since it is default)
#define SEARCH_OPT_COUNT 4

/*
 * Different types of character sets
 * EMPTY_SET - Set used when a password contains non printable characters
 * ALPHA_LOWER - Set of lower case alphabetic characters
 * ALPHA_UPPER - Set of upper case alphabetic characters
 * NUMERIC - Set of numeric characters (i.e 0 to 9)
 * ASCII   - Set of other ascii characters as outlined in specification
 */
typedef enum {
    EMPTY_SET = 0,
    ALPHA_LOWER = 1,
    ALPHA_UPPER = 2,
    NUMERIC = 4,
    ASCII_OTHER = 8
} CharacterSet;

/*
 * Different options to use when searching for a password match
 * Note that EXACT is used when we wish to search for an exact match
 */
typedef enum { EXACT, CASE_CHECK, LEET, DOUBLEUP, DIG_APPEND } SearchOption;

/* SearchDescriptor
 *
 * Stores information on how to search password files given on command line.
 *
 * options: Array of SearchOption's to use when searching password file
 * optCount: Number of search options
 * appendCount: Used to store the number of digits to append if options
 *              contains DIG_APPEND
 */
typedef struct {
    SearchOption* options;
    int optCount;
    int appendCount;
} SearchDescriptor;

/* Password
 *
 * Stores information about a given password string.
 *
 * password: Password string itself
 * guessCount: Current number of guesses made for this password
 *             when searching password list files
 * mathced: true if the password was matched in a given list file with
 *          the given search options, false otherwise.
 */
typedef struct {
    char* password;
    unsigned long guessCount;
    bool matched;
} Password;

/* PasswordList
 *
 * Stores a data read from a given password list file.
 *
 * passwords: Array of passwords strings
 * sourceFile: Name of file from which data was read
 * count: Number of password strings
 * contentValid: true if file contains all valid passwords and is
 *               non-empty, false otherwise.
 */
typedef struct {
    char** passwords;
    char* sourceFile;
    int passCount;
    bool contentValid;
} PasswordList;

/* PasswordSet
 *
 * Stores a list of PasswordList's.
 *
 * lists: Array of PasswordList pointers
 * count: Number of lists
 */
typedef struct {
    PasswordList** lists;
    int listCount;
} PasswordSet;

/* contains_whitespace()
 * ---------------------
 * Check if a string contains whitespace.
 *
 * string: String to check
 *
 * Returns: True if string contains one or more whitespace characters,
 *          false otherwise.
 */
bool contains_whitespace(char* string)
{
    int len = strlen(string);

    for (int i = 0; i < len; i++) {
        if (string[i] == ' ') {
            return true;
        }
    }

    return false;
}

/* contains_non_printable()
 * ------------------------
 * Check if string contains a non-printable ASCII character.
 *
 * string: String to check
 *
 * Returns: True if string contains one or more non-printable ASCII
 *          characters, false otherwise.
 */
bool contains_non_printable(char* string)
{
    int len = strlen(string);

    for (int i = 0; i < len; i++) {
        if (!isprint(string[i])) {
            return true;
        }
    }
    return false;
}

/* alpha_count()
 * -------------
 * Find the number of alphabetic characters in word.
 *
 * word: String we want alphabetic character count of
 *
 * Returns: Number of alphabetic characters in word.
 */
int alpha_count(char* word)
{
    int count = 0;
    int len = strlen(word);

    for (int i = 0; i < len; i++) {
        if (isalpha(word[i])) {
            count++;
        }
    }

    return count;
}

/* free_password_list()
 * --------------------
 * Free a PasswordList type
 *
 * list: PasswordList to free
 */
void free_password_list(PasswordList* list)
{
    for (int i = 0; i < list->passCount; i++) {
        free(list->passwords[i]);
    }
    free(list->passwords);
    free(list->sourceFile);
    free(list);
}

/* free_password_set()
 * -------------------
 * Free a PasswordSet type
 *
 * set: PasswordSet to free
 */
void free_password_set(PasswordSet* set)
{
    for (int i = 0; i < set->listCount; i++) {
        free_password_list(set->lists[i]);
    }
    free(set->lists);
    free(set);
}

/* free_string_array()
 * -----------------
 * Free an array of strings (char*)
 *
 * array: Array of strings to free
 * count: Number of strings in array
 */
void free_string_array(char** array, int count)
{
    for (int i = 0; i < count; i++) {
        free(array[i]);
    }
    free(array);
}

/* get_sets()
 * ----------
 * Get the set of characters present in password.
 * (e.g. Upper/lower case alphabetic, numeric etc)
 *
 * password: Password to get sets of
 *
 * Returns: A logical combination of the CharacterSet types present
 *          in password. For example, "123aBc" would result in
 *          get_sets() returning NUMERIC | ALPHA_LOWER | ALPHA_UPPER
 *
 * Errors: If passwords is found to contain no type of CharacterSet, then
 *         EMPTY_SET is returned and should be ignored.
 */
CharacterSet get_sets(char* password)
{
    int len = strlen(password);
    CharacterSet result = EMPTY_SET;

    for (int i = 0; i < len; i++) {
        if (isdigit(password[i])) {
            result |= NUMERIC;
        } else if (islower(password[i])) {
            result |= ALPHA_LOWER;
        } else if (isupper(password[i])) {
            result |= ALPHA_UPPER;
        } else if (isprint(password[i])) {
            result |= ASCII_OTHER;
        }
    }

    return result;
}

/* calc_set_size()
 * --------------
 * Calculate the size of the set of characters present in password
 *
 * password: Password to calculate set size of
 *
 * Returns: The size of the set of characters present
 *          in password as an integer
 *
 * Errors: If the password provided is NULL or contains non-ASCII
 *         characters, calc_set_size() will return -1
 */
int calc_set_size(char* password)
{
    if (password == NULL) {
        return -1;
    }
    CharacterSet sets = get_sets(password);

    int setSize = 0;

    if (sets & ALPHA_LOWER) {
        setSize += ALPHABET_SET_SIZE;
    }
    if (sets & ALPHA_UPPER) {
        setSize += ALPHABET_SET_SIZE;
    }
    if (sets & NUMERIC) {
        setSize += NUMERIC_SET_SIZE;
    }
    if (sets & ASCII_OTHER) {
        setSize += ASCII_OTHER_SET_SIZE;
    }

    return setSize;
}

/* calc_entropy()
 * --------------
 * Calculate the two different entropies (if applicable) for a given
 * password. Will always calculate E_1 = L* log_2(S) and additionally
 * E_2 = log_2(2 * n) if the password was matched.
 *
 * toCalc: Password to calculate entropy of
 *
 * Returns: Float value representing the minimum entropy of password i.e.
 *          min(E_1, E_2) if E_2 != 0, otherwise will just return E_1.
 */
float calc_entropy(Password* toCalc)
{
    float entropy1 = 0;
    float entropy2 = 0;
    int setSize = calc_set_size(toCalc->password);
    int len = strlen(toCalc->password);

    entropy1 = len * log2(setSize);

    if (toCalc->matched) {
        // password was matched from list so calculate E_2
        entropy2 = log2(2 * toCalc->guessCount);
    }

    float entropy = 0;

    // Make entropy the minimum of entropy1 and entropy2
    if (entropy2 == 0) {
        entropy = entropy1;
    } else if (entropy1 < entropy2) {
        entropy = entropy1;
    } else {
        entropy = entropy2;
    }
    return (floor(entropy * ROUND_FACTOR) / ROUND_FACTOR);
}

/* print_password_strength()
 * -------------------------
 * Print the strength rating of a given password
 *
 * password: Password object to check strength of
 *
 */
void print_password_strength(float entropy)
{
    printf("Password strength rating: ");

    if (entropy < ENTROPY_WEAK) {
        printf("very weak");
    } else if ((ENTROPY_WEAK <= entropy) && (entropy < ENTROPY_STRONG)) {
        printf("weak");
    } else if ((ENTROPY_STRONG <= entropy) && (entropy < ENTROPY_VERY_STRONG)) {
        printf("strong");
    } else if (ENTROPY_VERY_STRONG <= entropy) {
        printf("very strong");
    }
    printf("\n");
}

/* read_line()
 * ----------
 * Read a single line from an input file. A line will end at a newline
 * character or an EOF.
 *
 * file: File to read line from
 *
 * Returns: Pointer to line that was read
 *
 * Errors: If the file is empty or the input stream is closed, NULL will be
 *         returned.
 */
char* read_line(FILE* file)
{
    char* result = (char*)calloc(1, sizeof(char));
    int index = 0;

    // Check if file is empty
    if (feof(file)) {
        free(result);
        return NULL;
    }

    while (true) {
        char next = fgetc(file);

        // This check is used when reading from stdin to detect Crtl-D
        if ((next == EOF) && (strlen(result) == 0)) {
            free(result);
            return NULL;
        }

        if ((next == '\n') || (next == EOF)) {
            break;
        }
        // Add next to the current string
        result[index] = next;
        index++;
        result = (char*)realloc(result, (index + 1) * sizeof(char));
    }
    // terminate string
    result[index] = '\0';
    return result;
}

/* get_password_from_user()
 * ------------------------
 * Get a password from the user. Creates a Password type with string from user
 * and corresponding entropy
 *
 * Returns: Pointer to Password type containing user's input string and entropy
 *          of string
 *
 * Errors: If a valid password was not read i.e. empty input or contained
 *         whitespace or non-printable characters, then NULL is returned.
 *
 */
Password* get_password_from_user(void)
{
    Password* userPass = (Password*)malloc(sizeof(Password));
    userPass->matched = false;
    userPass->guessCount = 0;
    char* line = read_line(stdin);

    // stdin was closed by user using Crtl-D
    if (line == NULL) {
        free(userPass);
        return NULL;
    }

    // Asks for new password while given password contains whitespace,
    // non-printable or is empty
    while (contains_whitespace(line) || contains_non_printable(line)
            || (strlen(line) == 0)) {
        free(line);
        fprintf(stderr, "Password is not valid\n");
        line = read_line(stdin);
        if (line == NULL) {
            free(userPass);
            return NULL;
        }
    }

    userPass->password = line;
    return userPass;
}

/* arg_is_option()
 * --------------
 * Check if a command line argument is an option.
 *
 * arg: Argument to check
 *
 * Returns: True if arg is one of "--leet", "--casecheck", "--doubleup" or
 *          "--digit-append", false otherwise.
 */
bool arg_is_option(char* arg)
{
    bool result = false;

    if (strcmp(arg, "--leet") == 0) {
        result = true;
    } else if (strcmp(arg, "--digit-append") == 0) {
        result = true;
    } else if (strcmp(arg, "--casecheck") == 0) {
        result = true;
    } else if (strcmp(arg, "--doubleup") == 0) {
        result = true;
    } else if (atoi(arg)) {
        result = true;
    }
    return result;
}

/* cmdline_file_start()
 * --------------------
 * Find the index of argv which contains the first file name.
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 *
 * Returns: The index of argv which contains the first filename, -1 if
 *          argv does not contain any valid file names.
 *
 * Errors: If no files were passed through command line then 0 is returned
 *         and is not a valid value for the start of files in argv.
 */
int cmdline_file_start(int argc, char** argv)
{
    int start = 0;

    for (int i = 1; i < argc; i++) {
        if (!((argv[i][0] == '-') && (argv[i][1] == '-'))) {
            // Find a digit, check if it's part of --digit-append or
            // a file name
            if (atoi(argv[i])) {
                if (strcmp(argv[i - 1], "--digit-append") != 0) {
                    start = i;
                    break;
                }
            } else {
                start = i;
                break;
            }
        }
    }
    return start;
}

/* get_cmdline_file_count()
 * -----------------------
 * Get the number of file names passed in through command line. Note that
 * the first filename cannot begin with "--".
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 *
 * Returns: Number of files given through command line
 */
int cmdline_file_count(int argc, char** argv)
{
    int fileStart = cmdline_file_start(argc, argv);
    // No files
    if (fileStart == 0) {
        return 0;
    }
    return argc - fileStart;
}

/* cmdline_options_present()
 * ------------------------
 * Check if any option flags were passed through the command line by user.
 *
 * argc: Number of command line arguments passed
 * fileStart: Index of argv containing the first file name
 * argv: Array of command line arguments
 *
 * Returns: True if the second element in argv is a valid uqentropy option,
 *          false otherwise.
 */
bool cmdline_options_present(int argc, int fileStart, char** argv)
{
    if (argc == 1) {
        return false;
    }
    int endIndex = argc - fileStart - 1;

    for (int i = 1; i <= endIndex; i++) {
        if (!arg_is_option(argv[i])) {
            return false;
        }
    }

    return true;
}

/* arg_duplicated()
 * ---------------
 * Check if a command line argument was duplicated.
 *
 * fileStart: Index of argv which contains the first file name
 * argv: Array of command line arguments
 *
 * Returns: True if one or more elements in argv is repeated once or more,
 *          false otherwise.
 */
bool arg_duplicated(int fileStart, char** argv)
{
    char** argsChecked = (char**)malloc(sizeof(char*));
    int numArgsChecked = 0;

    for (int i = 1; i < fileStart; i++) {
        for (int j = 0; j < numArgsChecked; j++) {
            // argv[i] has already appeared once meaning argv[i] is duplicated
            if (strcmp(argv[i], argsChecked[j]) == 0) {
                free_string_array(argsChecked, numArgsChecked);
                return true;
            }
        }

        // Add argument to array of string checked
        argsChecked = (char**)realloc(
                argsChecked, (numArgsChecked + 1) * sizeof(char*));
        argsChecked[numArgsChecked] = strdup(argv[i]);
        numArgsChecked++;
    }

    free_string_array(argsChecked, numArgsChecked);
    return false;
}

/* verify_cmdline_args()
 * ---------------------
 * Verify that the given command line arguments are valid.
 *
 * argc: Number of command line arguments passed
 * argv: Array of command line arguments
 *
 * Returns: true if arguments are valid according to UQEntropy specification,
 *          false otherwise.
 */
bool verify_cmdline_args(int argc, char** argv)
{
    if (argc == 1) {
        return true; // no command line arguments
    }

    int fileCount = cmdline_file_count(argc, argv);
    int fileStart = cmdline_file_start(argc, argv);

    if (arg_duplicated(fileStart, argv)) {
        return false;
    }

    if (cmdline_options_present(argc, fileStart, argv)) {
        // If any options are present we require at least one file
        if (fileCount == 0) {
            return false;
        }
    }
    bool result = true;

    for (int i = 1; i < argc - fileCount; i++) {
        if (!arg_is_option(argv[i])) {
            return false;
        }
        // Check that --digit-append is followed by a digit that
        // is between 1 and 6 inclusive
        if (strcmp(argv[i], "--digit-append") == 0) {
            if (i == argc - 1) {
                result = false;
            } else if (((atoi(argv[i + 1]) < DIG_APPEND_MIN)
                               || (atoi(argv[i + 1]) > DIG_APPEND_MAX))) {
                result = false;
            }
        }
    }

    // Check for any empty strings in all arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "") == 0) {
            result = false; // empty string
        }
    }
    return result;
}

/* parse_cmdline_files()
 * ---------------------
 * Parse the file names passed to command line into an array of names.
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 *
 * Returns: Array of file names.
 */
char** parse_cmdline_files(int argc, char** argv)
{
    int fileStart = cmdline_file_start(argc, argv);
    char** fileNames = (char**)malloc(sizeof(char*));
    int nameCount = 0;

    for (int i = fileStart; i < argc; i++) {
        // Add file name to fileNames
        fileNames = (char**)realloc(fileNames, (nameCount + 1) * sizeof(char*));
        fileNames[nameCount] = strdup(argv[i]);
        nameCount++;
    }

    return fileNames;
}

/* split_line()
 * ------------
 * Split line into tokens based on split string. Used when parsing passwords
 * from a file.
 *
 * list: PasswordList to store resulting tokens in
 * line: Line from file to split
 * split: String to split line on
 */
void split_line(PasswordList* list, char* line, char* split)
{
    // split line on string split
    char* token = strtok(line, split);

    while (token != NULL) {
        // Add token to list's passwords array
        list->passwords = (char**)realloc(
                list->passwords, (list->passCount + 1) * sizeof(char*));
        list->passwords[list->passCount] = strdup(token);
        list->passCount++;
        token = strtok(NULL, split);
    }
}

/* read_passwords_from_file()
 * --------------------------
 * Read all passwords from pass_file into list.
 *
 * list: pointer to PasswordList to store read passwords
 * passFile: file to read passwords from
 *
 */
void read_passwords_from_file(PasswordList* list, FILE* passFile)
{
    char* line = read_line(passFile);

    while (line != NULL) {
        // empty line so just skip over
        if (strcmp(line, "") == 0) {
            free(line);
            line = read_line(passFile);
            continue;
        }
        if (contains_non_printable(line)) {
            list->contentValid = false;
        }
        if (contains_whitespace(line)) {
            split_line(list, line, " ");
        } else {
            // Password is valid so we add it to the lists array
            list->passwords = (char**)realloc(
                    list->passwords, (list->passCount + 1) * sizeof(char*));
            list->passwords[list->passCount] = strdup(line);
            list->passCount++;
        }
        free(line);
        line = read_line(passFile);
    }
}

/* parse_passwords()
 * -----------------
 * Parse passwords from file into PasswordList object.
 *
 * fileName: Name of file to parse passwords from
 *
 * Returns: Pointer to PasswordList containing parsed passwords
 *
 * Errors: If the input file could not be opened, then NULL is returned.
 */
PasswordList* parse_passwords(char* fileName)
{
    PasswordList* list = (PasswordList*)malloc(sizeof(PasswordList));
    list->passwords = (char**)malloc(sizeof(char*));
    FILE* passFile = fopen(fileName, "r");

    if (passFile == NULL) {
        fprintf(stderr, "uqentropy: unable to read from password file \"%s\"\n",
                fileName);
        free(list->passwords);
        free(list);
        return NULL;
    }

    list->sourceFile = strdup(fileName);
    list->passCount = 0;
    list->contentValid = true;

    // read all passwords from pass_file
    read_passwords_from_file(list, passFile);

    if ((list->passCount == 0)) {
        // we didn't read any passwords (empty file)
        fprintf(stderr, "uqentropy: no valid passwords found in file \"%s\"\n",
                fileName);
        list->contentValid = false;
    } else if (!list->contentValid) {
        // contentValid flag is set meaning invalid password found in file
        fprintf(stderr,
                "uqentropy: \"%s\" contains invalid password character\n",
                fileName);
    }

    fclose(passFile);
    return list;
}

/* parse_options()
 * ----------------
 * Parse options given through command line (e.g. --casecheck, --leet) into
 * a SearchDescriptor type.
 *
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 * fileStart: Index of argv where the first file name is
 *
 * Returns: A pointer to a SearchDescriptor type containing each search type
 *          given in command line arguments. Note by default the first search
 *          option of the result will be EXACT as we always want to search for
 *          an exact match.
 */
SearchDescriptor* parse_options(char** argv, int fileStart)
{
    SearchDescriptor* result
            = (SearchDescriptor*)malloc(sizeof(SearchDescriptor));
    result->options = (SearchOption*)malloc(sizeof(SearchOption));
    // Force first option to be EXACT since we will always at least search
    // for exact match
    result->options[0] = EXACT;
    result->optCount = 1;
    // order in which options should be parsed
    SearchOption order[] = {CASE_CHECK, DIG_APPEND, DOUBLEUP, LEET};

    for (int j = 0; j < SEARCH_OPT_COUNT; j++) {
        for (int i = 1; i < fileStart; i++) {
            if (atoi(argv[i])) {
                continue;
            }
            SearchOption toAdd = EXACT;
            if (strcmp(argv[i], "--casecheck") == 0) {
                toAdd = CASE_CHECK;
            }
            if (strcmp(argv[i], "--digit-append") == 0) {
                toAdd = DIG_APPEND;
                result->appendCount = atoi(argv[i + 1]);
            }
            if (strcmp(argv[i], "--doubleup") == 0) {
                toAdd = DOUBLEUP;
            }
            if (strcmp(argv[i], "--leet") == 0) {
                toAdd = LEET;
            }

            if (toAdd == order[j]) {
                // Add option to option array only if it is the correct order
                result->options = (SearchOption*)realloc(result->options,
                        (result->optCount + 1) * sizeof(SearchOption));
                result->options[result->optCount] = toAdd;
                result->optCount++;
            }
        }
    }
    return result;
}

/* read_in_passwords()
 * -------------------
 * Read in passwords from files given in command line to create PasswordSet.
 *
 * filesNames: Array of the names of files to read
 * fileCount: Number of names in file_names
 *
 * Returns: Pointer to a PasswordSet containing the all PasswordLists
 *          corresponding to each file.
 *
 * Errors: If any list in set is found to have the contentValid flag set
 *         to false, NULL is returned.
 */
PasswordSet* read_in_passwords(char** fileNames, int fileCount)
{
    PasswordSet* set = (PasswordSet*)malloc(sizeof(PasswordSet));
    set->lists = (PasswordList**)malloc(sizeof(PasswordList*));
    set->listCount = 0;
    bool setValid = true;

    for (int i = 0; i < fileCount; i++) {
        PasswordList* next = parse_passwords(fileNames[i]);

        // We found an invalid list, we set the setValid flag to false
        if ((next == NULL) || (!next->contentValid)) {
            setValid = false;
        }
        if (next != NULL) {
            // List may have invalid content but is not NULL so we must
            // read into memory
            set->lists = (PasswordList**)realloc(
                    set->lists, (set->listCount + 1) * sizeof(PasswordList*));
            set->lists[set->listCount] = next;
            set->listCount++;
        }
    }

    if (!setValid) {
        free_password_set(set);
        return NULL;
    }
    return set;
}

/* swap_char_leet()
 * ----------------
 * Swap a character according to LeetSpeak. Will return an array
 * of size two. The second element may be the null character '\0' if
 * swap only had one possible substitute character.
 *
 * swap: Character to find LeetSpeak swap characters for
 *
 * Returns: Pointer to array of size 2 containing one or two swap characters
 *          depending on if swap has 1 or 2 substitutes according to
 *          LeetSpeak.
 */
char* swap_char_leet(char swap)
{
    char* result = (char*)calloc(2, sizeof(char));

    if ((swap == 'a') || (swap == 'A')) {
        result[0] = '@';
        result[1] = '4';
    }
    if ((swap == 'b') || (swap == 'B')) {
        result[0] = '6';
        result[1] = '8';
    }
    if ((swap == 'e') || (swap == 'E')) {
        result[0] = '3';
    }
    if ((swap == 'g') || (swap == 'G')) {
        result[0] = '6';
        result[1] = '9';
    }
    if ((swap == 'i') || (swap == 'I')) {
        result[0] = '1';
        result[1] = '!';
    }
    if ((swap == 'l') || (swap == 'L')) {
        result[0] = '1';
    }
    if ((swap == 'o') || (swap == 'O')) {
        result[0] = '0';
    }
    if ((swap == 's') || (swap == 'S')) {
        result[0] = '5';
        result[1] = '$';
    }
    if ((swap == 't') || (swap == 'T')) {
        result[0] = '7';
        result[1] = '+';
    }
    if ((swap == 'x') || (swap == 'X')) {
        result[0] = '%';
    }
    if ((swap == 'z') || (swap == 'Z')) {
        result[0] = '2';
    }

    return result;
}

/* leet_sub_count()
 * ----------------
 * Get the number of letters in word which have a LeetSpeak substitution
 * count of subCount.
 *
 * word: Word to get count of
 * subCount: The number of LeetSpeak substitution characters the characters
 *            of word we wish to count.
 *
 * Returns: The number of characters in word which have subCount different
 *          substitutions according to LeetSpeak.
 *
 * Errors: If an invalid is given for subCount (i.e. not 1 or 2), then NULL
 *         is returned and should be ignored.
 */
int leet_sub_count(char* word, int subCount)
{
    int len = strlen(word);
    int count = 0;

    for (int i = 0; i < len; i++) {
        char* subs = swap_char_leet(word[i]);

        // both elements of subs are not null and we're looking for sub count
        // of two
        if ((subs[0] != '\0') && (subs[1] != '\0') && (subCount == 2)) {
            count++;
        } else if ((subs[0] != '\0') && (subs[1] == '\0') && (subCount == 1)) {
            // subs only has one valid element and we're looking for sub count
            // of one
            count++;
        }
        free(subs);
    }
    return count;
}

/* check_leet()
 * ------------
 * Check if password can be converted to candidate by means of
 * LeetSpeak substitution.
 *
 * candidate: Source password given by user
 * password: Password to check if it can be converted to candidate through
 *           LeetSpeak substitution.
 *
 * Returns: true if password can be converted to candidate by means of
 *          LeetSpeak substitution, false otherwise.
 */
bool check_leet(Password* candidate, char* password)
{
    int passLen = strlen(password);
    int candLen = strlen(candidate->password);
    int oneSub = leet_sub_count(password, 1);
    int twoSub = leet_sub_count(password, 2);
    // No leet-speak substitutable characters in password so return false
    if ((oneSub == 0) && (twoSub == 0)) {
        return false;
    }
    // Add 2^(oneSub)3^(twoSub) - 1 to guess count
    candidate->guessCount += (unsigned long)pow(LEET_COUNT_FIRST_BASE, oneSub)
                    * pow(LEET_COUNT_SECOND_BASE, twoSub)
            - 1;
    if (passLen != candLen) {
        // Passowrds aren't the same length, no way to match them using
        // leet speak.
        return false;
    }

    for (int i = 0; i < passLen; i++) {
        if (candidate->password[i] != password[i]) {
            // Candidate character doesn't match passowrd character but
            // character is not alphabetical (can't be substituted)
            if (!isalpha(password[i])) {
                return false;
            }
            char* leetSubs = swap_char_leet(password[i]);

            // Check if neither substitute matches the candidate password
            // characters
            if (!((leetSubs[0] == candidate->password[i])
                        || (leetSubs[1] == candidate->password[i]))) {
                free(leetSubs);
                return false;
            }
            free(leetSubs);
        }
    }
    return true;
}

/* check_dig_append()
 * ------------------
 * Check if password can append a number of digitCount to make it the
 * same string as candidate->password.
 *
 * candidate: Password to try to match
 * digitCount: Maximum number of digits to try to append to password to make
 *              it match candidate->password
 * password: Password to see appending digits to will make it match the
 *           candidate password.
 *
 * Returns: True if there exists a number with at most digitCount digits
 *          that can be appended to password to make it match the candidate
 *          password string, false otherwise.
 */
bool check_dig_append(Password* candidate, int digitCount, char* password)
{
    int candLen = strlen(candidate->password);
    if (isdigit(password[strlen(password) - 1])) {
        return false;
    }
    // Remove trailing digits from candidate
    for (int i = 1; i <= digitCount; i++) {
        char* candPass = candidate->password;

        if (!isdigit(candPass[strlen(candPass) - 1])) {
            // Last char isn't digit, no point continuing just add to count
            break;
        }
        char* candNoDigit = strdup(candPass);
        // Remove i number of digits from candPass
        candNoDigit[candLen - i] = '\0';
        // Make candNum point to the number on the end of the candidate
        // password.
        char* candNum = candidate->password + candLen - i;

        if (alpha_count(candNum) != 0) {
            // Ending characters are not all digits, no point checking
            free(candNoDigit);
            break;
        }
        if (strcmp(password, candNoDigit) == 0) {
            // Add to password guess count
            candidate->guessCount += (unsigned long)atoi(candNum) + 1;
            int candNumLen = strlen(candNum);

            for (int i = 1; i < candNumLen; i++) {
                candidate->guessCount
                        += (unsigned long)pow(DIG_APPEND_COUNT_BASE, i);
            }
            free(candNoDigit);
            return true;
        }
        free(candNoDigit);
    }
    // Didn't find a match so add 10+10^2+...+10^n (n is number of digits
    // to append)
    for (int i = 1; i <= digitCount; i++) {
        candidate->guessCount += (unsigned long)pow(DIG_APPEND_COUNT_BASE, i);
    }
    return false;
}

/* get_all_passwords()
 * -------------------
 * Condense all passwords within a PasswordSet into a single array.
 * Helper function for check_double_up().
 *
 * set: PasswordSet to condense
 * count: Pointer to variable to keep track of total number of passwords
 *
 * Returns: Pointer to array of password strings.
 */
char** get_all_passwords(PasswordSet* set, int* count)
{
    char** result = (char**)malloc(sizeof(char*));
    *count = 0;

    for (int i = 0; i < set->listCount; i++) {
        for (int j = 0; j < set->lists[i]->passCount; j++) {
            PasswordList* list = set->lists[i];
            int passLen = strlen(list->passwords[j]);
            // Add password to result array
            result = (char**)realloc(result, ((*count) + 1) * sizeof(char*));
            result[*count] = (char*)malloc((passLen + 1) * sizeof(char));
            strcpy(result[*count], list->passwords[j]);
            (*count)++;
        }
    }
    return result;
}

/* check_double_up()
 * -----------------
 * Check if any combination of two passwords from set can be
 * concatenated to match candidate->password.
 *
 * candidate: Password to try to match
 * set: Set of passwords read from password files
 *
 * Returns: True if there is atleast one combination of two passwords
 *          from set which can be concatenated to match candidate->password,
 *          false otherwise.
 */
bool check_double_up(Password* candidate, PasswordSet* set)
{
    int passTotal = 0;
    char** passwords = get_all_passwords(set, &passTotal);

    for (int i = 0; i < passTotal; i++) {
        char* candPass = strdup(candidate->password);
        char* subString = strstr(candPass, passwords[i]);
        // Check if substring was not present
        if (subString == NULL) {
            free(candPass);
            continue;
        }
        char* search = NULL;

        if (strcmp(subString, candPass) == 0) {
            // candPass begins with passwords[i] so make
            // search point to rest of candPass
            search = subString + strlen(passwords[i]);
        } else {
            // passwords[i] must've been at start or somewhere in
            // the middle of candPass so don't search
            free(candPass);
            continue;
        }

        for (int j = 0; j < passTotal; j++) {
            if (strcmp(passwords[j], search) == 0) {
                // We found the search string in passwords. To get here we
                // would've had to check passTotal * i many combinations before
                // getting to passwords[i]. After getting to passwords[i] we
                // then had to check j + 1 more combinations (+1 for the one
                // that matches).
                candidate->guessCount
                        += (unsigned long)i * (unsigned long)passTotal
                        + (unsigned long)j + 1;
                free(candPass);
                free_string_array(passwords, passTotal);
                return true;
            }
        }
        free(candPass);
    }
    free_string_array(passwords, passTotal);
    // Didn't find match so and passTotal^2 to guessCount.
    candidate->guessCount += pow(passTotal, 2);
    return false;
}

/* search_for_match()
 * ------------------
 * Search for a match to candidate from set with search options in descriptor.
 *
 * candidate: Password to search for a match to
 * set: Password set to try to find match to candidate in
 * descriptor: SearchDescriptor to determine which options to search with
 *
 * Returns: true if a match to candidate was found in set with the given
 *          options in descriptor, false otherwise.
 */
bool search_for_match(
        Password* candidate, PasswordSet* set, SearchDescriptor* descriptor)
{
    // loop through options in descriptor
    for (int opt = 0; opt < descriptor->optCount; opt++) {
        SearchOption option = descriptor->options[opt];

        if (option == DOUBLEUP) {
            if (check_double_up(candidate, set)) {
                return true;
            }
        }
        for (int i = 0; i < set->listCount; i++) {
            // loop through each password in each list in set
            for (int j = 0; j < set->lists[i]->passCount; j++) {
                char* password = set->lists[i]->passwords[j];

                if ((option == CASE_CHECK) && (alpha_count(password) > 0)) {
                    // Add to guess count and use strcasecmp() to do case
                    // insensitive comparison
                    candidate->guessCount += pow(2, alpha_count(password)) - 1;
                    if (strcasecmp(candidate->password, password) == 0) {
                        return true;
                    }
                }
                if (option == LEET) {
                    if (check_leet(candidate, password)) {
                        return true;
                    }
                }
                if (option == DIG_APPEND) {
                    if (check_dig_append(
                                candidate, descriptor->appendCount, password)) {
                        return true;
                    }
                }
                if (option == EXACT) {
                    candidate->guessCount++;
                    if (strcmp(candidate->password, password) == 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/* handle_user_input()
 * -------------------
 * Handles user input password to check for match can calculate entropy.
 *
 * set: PasswordSet to use for searching
 * desc: SearchDescriptor search options to use when searching for match
 * strongEntered: Flag to set if a strong password is entered by user
 * fileCount: File count given in command line
 *
 * Returns: true if stdin is not closed by user, false otherwise
 */
bool handle_user_input(PasswordSet* set, SearchDescriptor* desc,
        bool* strongEntered, int fileCount)
{
    Password* user = get_password_from_user();
    // user closed stdin so return false
    if (user == NULL) {
        return false;
    }
    if (fileCount > 0) {
        if (search_for_match(user, set, desc)) {
            user->matched = true;
            printf("Candidate password matched on guess number %lu\n",
                    user->guessCount);
        } else {
            printf("No match would be found after checking %lu passwords\n",
                    user->guessCount);
        }
    }
    float entropy = calc_entropy(user);
    if (entropy > ENTROPY_STRONG) {
        // Set strongEntered flag
        *strongEntered = true;
    }
    printf("Password entropy is %.1f\n", entropy);
    print_password_strength(entropy);
    free(user->password);
    free(user);
    return true;
}

int main(int argc, char** argv)
{
    // check cmdline argument validity
    if (!verify_cmdline_args(argc, argv)) {
        fprintf(stderr, "Usage: ./uqentropy [--leet] [--casecheck] ");
        fprintf(stderr, "[--digit-append 1..6] [--doubleup] [filename ...]\n");
        exit(EXIT_INVALID_USAGE);
    }

    int fileCount = cmdline_file_count(argc, argv);
    SearchDescriptor* desc = parse_options(argv, argc - fileCount);
    PasswordSet* set = NULL;
    bool strongEntered = false;

    if (fileCount > 0) {
        // read in passwords
        char** files = parse_cmdline_files(argc, argv);
        set = read_in_passwords(files, fileCount);

        free_string_array(files, fileCount);
        // read_in_passwords() failed so free and exit
        if (set == NULL) {
            free(desc->options);
            free(desc);
            exit(EXIT_FILE_ERROR);
        }
    }
    printf("Welcome to UQEntropy\n");
    printf("Written by s4834848.\n");
    printf("Enter possible password to check its strength.\n");

    // repeatedly run handle_user_input()
    while (true) {
        if (!handle_user_input(set, desc, &strongEntered, fileCount)) {
            break;
        }
    }

    if (set != NULL) {
        free_password_set(set);
    }
    free(desc->options);
    free(desc);
    if (!strongEntered) {
        printf("No strong password(s) entered\n");
        exit(EXIT_NO_STRONG_PASSWORD);
    }
    exit(EXIT_SUCCESS);
}
