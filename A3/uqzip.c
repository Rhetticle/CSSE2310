#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <csse2310a3.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <libgen.h>

// Macro to make a number x divisible by 4
#define MAKE_DIV_BY_FOUR(x) ((((x) + 3) / 4) * 4)

// Size of chunks of data read from pipe at a time
#define READ_BUFF_SIZE 1024

// Number of children in a pair when executing parallel decompression
#define PARALLEL_DECOMP_PAIR_SIZE 2

// Byte number at which record offsets begin in UQZ header
#define OFFSET_START 8

// Constant to represent that a worker is running (compressing or
// decompressing)
#define WORKER_RUNNING 4

// Error/Status Codes
#define EXIT_OK 0
#define WRITE_ERROR 3
#define READ_ERROR 19
#define SIGNAL_ERROR 8
#define COMMAND_ERROR 20
#define USAGE_ERROR 9
#define FORMAT_ERROR 2
#define INTERRUPT_ERROR 5

// Global variable to show that SIGINT has been caught
bool sigIntCaught = false;

/* Look up table for compressed and decompression commands to use in
 * conjunction with CompMethod constants. e.g. compCommandLUT[XZ] = 'xz' etc.
 */
const char* const compCommandLUT[]
        = {NULL, "cat", "bzip2", "gzip", "xz", "zip"};
const char* const deCompCommandLUT[]
        = {NULL, "cat", "bzip2", "gzip", "xz", "funzip"};

/* CompMethod
 *
 * Different types of compression/decompression options.
 *
 * EMPTY: Used to signify that no option has been specified
 * DECOMP: Decompress
 * NOCOMP: No compression (use "cat")
 * BZIP2: Use "bzip2" for compression/decompression
 * GZ: Use "gzip" for compression/decompression
 * XZ: Use "xz" for compression/decompression
 * ZIP: Use "zip" for compression/ "funzip" for decompression
 */
typedef enum { EMPTY, NOCOMP, BZIP2, GZ, XZ, ZIP, DECOMP } CompMethod;

/* Parameters
 *
 * Stores information about options and input/output files passed to
 * program by command line.
 *
 * method: Compression/decompression method to use.
 * parallel: True if compression is to take place with parallel processes,
 *           false for sequential processes.
 * outName: Output file name (default is "out.uqz")
 * inputFiles: Array of input files to be compressed (NULL if decompressing)
 * inputCount: Number of input files.
 * archive: Name of archive file if decompressing (NULL if compressing)
 */
typedef struct {
    CompMethod method;
    bool parallel;
    bool decompress;
    char* outName;
    char** inputFiles;
    int inputCount;
    char* archive;
} Parameters;

/* Compressed
 *
 * Stores raw compressed data of a file.
 *
 * orgFile: Name of original file which is about to be compressed or
 *          name of file which is being extracted when decompressing.
 * data: Raw data (bytes) of compressed file
 * size: Number of bytes in data
 */
typedef struct {
    char* orgFile;
    uint8_t* data;
    uint32_t size;
} Compressed;

/* Worker
 *
 * Stores information about a uqzip child process for either compression
 * or decompression.
 *
 * pid: Process ID of the worker
 * parallel: True if worker is working on a parallel job, false if sequential
 * decompress: True if worker is decompressing, false if compressing
 * workingOn: String of file which worker is working on
 * state: Current state of the worker. Can be either WORKER_IDLE, WORKER_RUNNING
 * or, if it has exited, the reason for exiting e.g. EXIT_OK, SIGNAL_ERROR etc.
 */
typedef struct {
    pid_t pid;
    bool parallel;
    bool decompress;
    char* workingOn;
    int state;
} Worker;

/* free_parameters()
 * -----------------
 * Free a Parameters struct.
 *
 * param: Parameters struct to free
 */
void free_parameters(Parameters* param)
{
    if (param->outName) {
        free(param->outName);
    }
    for (int i = 0; i < param->inputCount; i++) {
        free(param->inputFiles[i]);
    }
    free(param->inputFiles);
    if (param->archive) {
        free(param->archive);
    }
    free(param);
}

/* free_compressed()
 * -----------------
 * Free a Compressed struct type.
 *
 * comp: Pointer to compressed struct to free.
 */
void free_compressed(Compressed* comp)
{
    if (comp->orgFile) {
        free(comp->orgFile);
    }
    if (comp->data) {
        free(comp->data);
    }
    free(comp);
}

/* free_worker()
 * -------------
 * Free the memory allocated to a Worker type.
 *
 * work: Pointer to Worker to free
 */
void free_worker(Worker* work)
{
    if (work->workingOn) {
        free(work->workingOn);
    }
    free(work);
}

/* free_workers()
 * --------------
 * Free memory allocated to an array of Workers.
 *
 * workers: Array of pointers to Worker types to free
 * size: Number of pointers in workers
 */
void free_workers(Worker** workers, int size)
{
    for (int i = 0; i < size; i++) {
        free_worker(workers[i]);
    }
    free(workers);
}

/* free_worker_pairs()
 * -------------------
 * Free memory allocated to an array of worker pairs. Note this is used only
 * in decompress_parallel() and assumes workers has been statically allocated
 * and thus only the pointers within workers need to be freed via free_worker()
 *
 * workers: Array of pointers to worker pairs
 * active: Number of pointers with workers which point to active (initialised)
 *         pairs of workers.
 */
void free_worker_pairs(Worker* (*workers)[2], int active)
{
    for (int i = 0; i < active; i++) {
        free_worker(workers[i][0]);
        free_worker(workers[i][1]);
    }
}

/* get_path()
 * ----------
 * Get the appropriate path from an absolute path to use when printing error
 * messages or compressing files.
 *
 * absPath: Absolute path to file
 *
 * Returns: Allocated string of either the basename of absPath if absPath does
 *          not end in '/' OR a copy of absPath if absPath ends in '/'.
 */
char* get_path(char* absPath)
{
    char* absDup = strdup(absPath);

    if (absPath[strlen(absPath) - 1] == '/') {
        return absDup;
    }
    // absPath doesn't end in '/' so use basename
    char* base = strdup(basename(absDup));
    free(absDup);
    return base;
}

/* arg_is_opt()
 * ------------
 * Check if a string is a command line option.
 *
 * arg: Argument to check
 *
 * Returns: True if arg begins with "--", false otherwise.
 */
bool arg_is_opt(const char* arg)
{
    if ((arg[0] == '-') && (arg[1] == '-')) {
        return true;
    }
    return false;
}

/* set_method()
 * ------------
 * Set the method of param based on the string arg from command line.
 *
 * arg: String to use to set method of param
 * param: Parameters struct to set method of
 *
 * Returns: True if arg is a valid method option e.g. "--xz", "--gz" etc and
 *          param->method can be set, false otherwise.
 */
bool get_method(char* arg, Parameters* param)
{
    if (param->method != EMPTY) {
        return false;
    }

    if (!strcmp(arg, "--gz")) {
        param->method = GZ;
    } else if (!strcmp(arg, "--zip")) {
        param->method = ZIP;
    } else if (!strcmp(arg, "--xz")) {
        param->method = XZ;
    } else if (!strcmp(arg, "--bzip2")) {
        param->method = BZIP2;
    } else if (!strcmp(arg, "--nocomp")) {
        param->method = NOCOMP;
    } else {
        return false;
    }
    return true;
}

/* parse_option()
 * --------------
 * Parse an option argument into param.
 *
 * param: Parameters struct to parse option into
 * argc: Number of command line arguments
 * argv: Array of command line arguments
 * index: Index of argv at which option to parse is located
 *
 * Returns: True if argv[*index] is a valid option that can be parsed into the
 *          relevant attribute of param, false otherwise.
 */
bool parse_option(Parameters* param, int argc, char** argv, int* index)
{
    if ((!strcmp(argv[*index], "--output")) && (param->outName == NULL)) {
        if ((*index == argc - 1) || (!argv[*index + 1][0])) {
            return false; // no name following --output option
        }
        param->outName = strdup(argv[(*index) + 1]);
        (*index)++;
    } else if ((!strcmp(argv[*index], "--parallel")) && (!param->parallel)) {
        param->parallel = true;
    } else if (!strcmp(argv[*index], "--decompress")) {
        param->decompress = true;
        param->method = DECOMP;
    } else if (!get_method(argv[*index], param)) {
        return false; // unknown or repeated option
    }
    return true;
}

/* process_cmdline_args()
 * ----------------------
 * Parse and process command line arguments into a Parameters struct.
 *
 * argc: Number of command line arguments.
 * argv: Array of command line arguments.
 *
 * Returns: Pointer to a Parameters struct which contains the relevant data
 *          passed by command line OR NULL if the arguments were not valid
 *          (usage error).
 */
Parameters* process_cmdline_args(int argc, char** argv)
{
    Parameters* param = calloc(1, sizeof(Parameters));

    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) {
            free_parameters(param);
            return NULL; // empty string
        }
        if (arg_is_opt(argv[i]) && !param->inputCount) {
            if (!parse_option(param, argc, argv, &i)) {
                free_parameters(param);
                return NULL;
            }
        } else {
            // wasn't an option
            if (param->method == DECOMP) {
                if (param->archive != NULL) {
                    free_parameters(param);
                    return NULL; // multiple archive names given
                }
                param->archive = strdup(argv[i]);
            } else {
                // argv[i] wasn't an option and we aren't decompressing so
                // must be a filename so add to param->inputFiles
                int count = param->inputCount;
                param->inputFiles = realloc(
                        param->inputFiles, (count + 1) * sizeof(char**));
                param->inputFiles[count] = strdup(argv[i]);
                param->inputCount++;
            }
        }
    }
    if (((param->method == DECOMP)
                && ((param->outName != NULL) || (param->archive == NULL)))
            || ((param->method != DECOMP) && (param->inputCount == 0))) {
        // Either --decompress was given with no archive name or with an output
        // name OR no input file(s) were given for a compression
        free_parameters(param);
        return NULL;
    }
    // no decompression/compression option was given so set to default (NOCOMP)
    if (param->method == EMPTY) {
        param->method = NOCOMP;
    }
    return param;
}

/* write_header_section()
 * ----------------------
 * Write the header section of a .uqz archive output.
 *
 * param: Parameters struct containing relevant data from command line
 *        arguments.
 *
 * Returns: True if header section was successfully written to output archive,
 *          false if output archive could not be opened for writing.
 */
bool write_header_section(Parameters* param)
{
    // no output filename given so set to "out.uqz"
    if (param->outName == NULL) {
        param->outName = strdup("out.uqz");
    }
    // attempt to open output archive for reading and writing
    int outfd = open(param->outName, O_RDWR | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    if (outfd == -1) {
        fprintf(stderr, "uqzip: unable to open file \"%s\" for writing\n",
                param->outName);
        return false;
    }
    uint32_t* placeHolders = malloc(sizeof(uint32_t) * param->inputCount);
    memset(placeHolders, 0, sizeof(uint32_t) * param->inputCount);
    char* uqz = "UQZ";

    // write out header data in order of appearance e.g. "UQZ" tag first, then
    // method etc
    write(outfd, uqz, strlen(uqz));
    write(outfd, &param->method, 1);
    write(outfd, &param->inputCount, sizeof(uint32_t));
    write(outfd, placeHolders, sizeof(uint32_t) * param->inputCount);
    free(placeHolders);
    close(outfd);
    return true;
}

/* init_worker()
 * -------------
 * Initialise a worker from parameters from command line.
 *
 * param: Parameters formed from arguments passed by command line
 * workOn: Name of file for worker to work on
 *
 * Returns: Pointer to initialised Worker type
 */
Worker* init_worker(Parameters* param, char* workOn)
{
    Worker* work = malloc(sizeof(Worker));
    // Initialise attributes of worker based on param
    work->pid = -1;
    work->parallel = param->parallel;
    work->workingOn = strdup(workOn);
    work->decompress = param->decompress;
    work->state = WORKER_RUNNING;
    return work;
}

/* exec_worker()
 * -------------
 * Execute worker with a new command/program.
 *
 * work: Worker to execute
 * method: Compression method being used by worker
 *
 * Returns: EXEC_ERROR (-1) if execlp call fails
 */
int exec_worker(Worker* work, CompMethod method)
{
    if (!work->decompress) {
        switch (method) {
        case NOCOMP:
            execlp("cat", "cat", work->workingOn, NULL);
            break;
        case GZ:
            execlp("gzip", "gzip", "-n", "--best", "--stdout", work->workingOn,
                    NULL);
            break;
        case ZIP:
            execlp("zip", "zip", "-DXj", "-fz-", "-", work->workingOn, NULL);
            break;
        case XZ:
            execlp("xz", "xz", "--stdout", work->workingOn, NULL);
            break;
        case BZIP2:
            execlp("bzip2", "bzip2", "--stdout", work->workingOn, NULL);
            break;
        default:
            break;
        }
    } else {
        switch (method) {
        case NOCOMP:
            execlp("cat", "cat", NULL);
            break;
        case GZ:
            execlp("gzip", "gzip", "-dc", NULL);
            break;
        case ZIP:
            execlp("funzip", "funzip", NULL);
            break;
        case XZ:
            execlp("xz", "xz", "-dc", NULL);
            break;
        case BZIP2:
            execlp("bzip2", "bzip2", "-dc", NULL);
            break;
        default:
            break;
        }
    }
    // should only get here if an execlp call fails
    return -1;
}

/* empty_pipe()
 * ------------
 * Read all data from a given pipe used for relaying compressed file data.
 *
 * readfd: Reading file descriptor for pipe.
 *
 * Returns: Pointer to Compressed struct which contains compressed file data.
 */
Compressed* empty_pipe(int readfd)
{
    Compressed* result = calloc(1, sizeof(Compressed));
    uint8_t* data = malloc(sizeof(uint8_t));
    uint8_t buffer[READ_BUFF_SIZE];
    int actualSize;
    uint32_t totalRead = 0;

    // Continously read at most READ_BUFF_SIZE from the pipe into a buffer
    // and add to data array
    while ((actualSize = read(readfd, buffer, READ_BUFF_SIZE)) > 0) {
        data = realloc(data, (totalRead + actualSize) * sizeof(uint8_t));
        memcpy(data + totalRead, buffer, actualSize);
        totalRead += actualSize;
    }
    result->data = data;
    result->size = totalRead;
    return result;
}

/* enter_record()
 * --------------
 * Enter a file record for a given Compressed item.
 *
 * comp: Compressed item to write record for.
 * param: Command line parameters:
 * inIndex: Index of param->inputFiles[] at which the file we wish to enter
 *        a record for is located.
 */
void enter_record(Compressed* comp, Parameters* param, int inIndex)
{
    // Use fd instead of FILE* so that we can use lseek to get file length
    int outfd = open(param->outName, O_RDWR, 0);
    uint32_t fileLen = lseek(outfd, 0, SEEK_END);
    char* baseFile = get_path(param->inputFiles[inIndex]);
    uint8_t inLength = strlen(baseFile);
    // Offset of the new file is just the current file length before we add
    // the new file
    uint32_t offset = fileLen;
    uint32_t recordSize
            = sizeof(comp->size) + sizeof(inLength) + inLength + comp->size;
    int padCount = MAKE_DIV_BY_FOUR(recordSize) - recordSize;
    uint8_t nullByte = 0;
    // Enter file record data fields
    write(outfd, &(comp->size), sizeof(uint32_t));
    write(outfd, &inLength, sizeof(uint8_t));
    write(outfd, baseFile, inLength);
    write(outfd, comp->data, comp->size);

    for (int i = 0; i < padCount; i++) {
        write(outfd, &nullByte, sizeof(uint8_t)); // Add padding
    }
    // Fix offsets in header
    lseek(outfd, OFFSET_START + sizeof(uint32_t) * inIndex, SEEK_SET);
    write(outfd, &offset, sizeof(uint32_t));
    free(baseFile);
    close(outfd);
}

/* read_record()
 * -------------
 * Read a file record from an archive file.
 *
 * archive: Archive file to read record
 * header: Header of archive file
 * fileIndex: File number in archive file to read record of
 *
 * Returns: Pointer to Compressed type containing file record information
 */
Compressed* read_record(FILE* archive, UqzHeaderSection* header, int fileIndex)
{
    Compressed* result = calloc(1, sizeof(Compressed));
    uint32_t offset = header->fileRecordOffsets[fileIndex];
    uint32_t dataSize;
    uint8_t nameLen;

    fseek(archive, offset, SEEK_SET);
    if ((fread(&dataSize, sizeof(uint32_t), 1, archive) != 1)
            || (fread(&nameLen, sizeof(uint8_t), 1, archive) != 1)) {
        // We either didn't read a 4 byte data size field or didn't read
        // a 1 byte name length field.
        free_compressed(result);
        return NULL;
    }

    result->orgFile = malloc((nameLen + 1) * sizeof(char));
    if (fread(result->orgFile, sizeof(char), nameLen, archive) != nameLen) {
        // Didn't read nameLen number of bytes from name field of file record
        free_compressed(result);
        return NULL;
    }
    result->orgFile[nameLen] = '\0'; // add terminator
    result->data = malloc(dataSize * sizeof(uint8_t));
    result->size = dataSize;

    if (fread(result->data, sizeof(uint8_t), dataSize, archive) != dataSize) {
        // Didn't read dataSize number of bytes from data field of file record
        free_compressed(result);
        return NULL;
    }
    return result;
}

/* suppress_output()
 * -----------------
 * Suppress the output of a process such as stdout or stderr.
 *
 * suppressfd: File descriptor of output to suppress
 */
void suppress_output(int suppressfd)
{
    int fdnull = open("/dev/null", O_WRONLY, 0);
    dup2(fdnull, suppressfd);
    close(fdnull);
}

/* redirect_worker()
 * -----------------
 * Redirect a worker's stdout, stdin and stderr depending on what the worker
 * is doing e.g. compression, decompression etc
 *
 * work: Pointer to worker to redirect
 * pipefd: File descriptors of pipe connected between worker and parent
 */
void redirect_worker(Worker* work, int pipefd[2])
{
    if (!work->decompress) {
        // must be a compression worker so suppress stderr and redirect
        // stdout to write end of pipe
        close(pipefd[0]);
        suppress_output(STDERR_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
    } else {
        // close write end of pipe and suppress stderr
        close(pipefd[1]);
        suppress_output(STDERR_FILENO);
        int outfd = open(work->workingOn, O_RDWR | O_TRUNC | O_CREAT,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        // redirect stdout to out file descriptor and stdin to read end of pipe
        dup2(outfd, STDOUT_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(outfd);
        close(pipefd[0]);
    }
}

/* get_exit_reason()
 * -----------------
 * Get the reason for a worker death for a given status. This function will set
 * the worker state from WORKER_RUNNING to the reason for death (either
 * SIGNAL_ERROR or COMMAND_ERROR).
 *
 * work: Worker to get exit reason of
 * status: Integer value set by wait() or waitpid()
 * method: Compression/decompression method which work was using
 *
 * Returns: true if worker successfully exited (i.e. exit status 0) otherwise
 *          false.
 */
bool get_exit_reason(Worker* work, int status, CompMethod method)
{
    const char* command;
    bool result = true;
    // set the worker's state initially to EXIT_OK (no longer WORKER_RUNNING)
    work->state = EXIT_OK;

    if (work->decompress) {
        command = deCompCommandLUT[method];
    } else {
        command = compCommandLUT[method];
    }

    if ((WIFSIGNALED(status)) && (WTERMSIG(status) == SIGUSR1)) {
        // worker was signalled SIGUSR1
        fprintf(stderr, "uqzip: Unable to execute command \"%s\"\n", command);
        work->state = SIGNAL_ERROR;
        result = false;
    } else if ((WIFEXITED(status)) && (WEXITSTATUS(status))) {
        // worker was not signalled SIGTERM but didn't exit with status 0
        char* path = get_path(work->workingOn);
        fprintf(stderr, "uqzip: \"%s\" command failed for filename \"%s\"\n",
                command, path);
        work->state = COMMAND_ERROR;
        free(path);
        result = false;
    }
    return result;
}

/* verify_extractable()
 * --------------------
 * Verify that a record within a .uqz archive is extractable. This function
 * will ensure that the record is valid and that the filename from the record
 * can be opened for writing.
 *
 * archive: Name of .uqz archive to verify
 * header: Header section of the archive
 * record: Index of record to verify, 0 is first record, 1 is second etc
 *
 * Returns: EXIT_OK if record is extractable OR FORMAT_ERROR if the
 *          record is not in the correct format OR WRITE_ERROR if the filename
 *          in the record cannot be opened for writing.
 */
int verify_extractable(char* archive, UqzHeaderSection* header, int record)
{
    FILE* archiveFile = fopen(archive, "r");
    Compressed* extract = read_record(archiveFile, header, record);
    fclose(archiveFile);

    // invalid record
    if (extract == NULL) {
        fprintf(stderr, "uqzip: File \"%s\" has invalid format\n", archive);
        return FORMAT_ERROR;
    }

    FILE* output = fopen(extract->orgFile, "w");

    // couldn't be opened for writing
    if (output == NULL) {
        fprintf(stderr, "uqzip: unable to open file \"%s\" for writing\n",
                extract->orgFile);
        free_compressed(extract);
        return WRITE_ERROR;
    }
    free_compressed(extract);
    fclose(output);
    return 0;
}

/* reap_worker()
 * -------------
 * Clean up a worker (child process) once it has exited or been signaled and
 * sets the worker's state accordingly.
 *
 * work: Pointer to worker to reap
 * method: Compression/decompression method being used by worker
 *
 * Returns: true if worker exited normally with exit status 0 false otherwise
 */
bool reap_worker(Worker* work, CompMethod method)
{
    int status;
    waitpid(work->pid, &status, 0);

    return get_exit_reason(work, status, method);
}

/* signal_workers()
 * ----------------
 * Signal a group of workers with a specific signal.
 *
 * workers: Array of pointers to workers to signal
 * startIndex: Array position of workers array to begin signalling from
 * size: Number of elements in workers array
 * signal: Signal to issue to workers e.g. SIGTERM, SIGINT etc
 */
void signal_workers(Worker** workers, int startIndex, int size, int signal)
{
    for (int i = startIndex; i < size; i++) {
        // Issue signal to given child process, we don't care about their
        // exit status so pass NULL to waitpid
        if (workers[i]->state == WORKER_RUNNING) {
            kill(workers[i]->pid, signal);
            waitpid(workers[i]->pid, NULL, 0);
        }
    }
}

/* signal_and_remove_worker_pairs()
 * --------------------------------
 * Signal a group of worker pairs to terminate and remove the files they were
 * working on.
 *
 * workers: Group of worker pairs to signal and remove
 * size: Number of pairs in workers array
 */
void signal_and_remove_worker_pairs(Worker* (*workers)[2], int size)
{
    for (int i = 0; i < size; i++) {
        // signal this pair with SIGTERM
        signal_workers(workers[i], 0, PARALLEL_DECOMP_PAIR_SIZE, SIGTERM);

        if ((workers[i][0]->state == WORKER_RUNNING)
                || (workers[i][1]->state == WORKER_RUNNING)) {
            remove(workers[i][0]->workingOn);
        }
    }
}

/* start_worker()
 * --------------
 * Start a worker on a specific job/task. The calling process is forked and the
 * child PID is assigned to work->pid. The worker is then redirected and
 * executed.
 *
 * work: Worker to start
 * method: Compression/decompression method for worker to use
 * pipefds: File descriptors of pipe between worker and parent (uqzip)
 */
void start_worker(Worker* work, CompMethod method, int pipefds[2])
{
    // redirect stderr,stdout and stdin of worker and exec
    redirect_worker(work, pipefds);
    if (exec_worker(work, method) == -1) {
        raise(SIGUSR1);
    }
}

/* start_worker_pair()
 * -------------------
 * Start a pair of workers to work together. Used when decompressing in
 * parallel.
 *
 * pair: Array of the two pointers of the worker pair
 * comp: Compressed data read from an archive file to be decompressed
 * method: Decompression method to be used to extract data
 * pipefds: File descriptors of pipe between the worker pair
 */
void start_worker_pair(
        Worker* pair[2], Compressed* comp, CompMethod method, int pipefds[2])
{
    // fork the worker which will send compression data to other worker
    pair[1]->pid = fork();

    if (!pair[1]->pid) {
        // send data to decompression child (pair[0])
        close(pipefds[0]);
        write(pipefds[1], comp->data, comp->size);
        close(pipefds[1]);
        exit(EXIT_OK);

    } else if (!(pair[0]->pid = fork())) {
        // start pair[0] as a normal decompression worker
        start_worker(pair[0], method, pipefds);
    }
}

/* create_empty_file()
 * -------------------
 * Create an empty file with name fileName.
 *
 * fileName: Name to give empty file
 */
void create_empty_file(char* fileName)
{
    int emptyfd = open(fileName, O_RDWR | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    close(emptyfd);
}

/* sigInt_clean_up()
 * -----------------
 * Wait for child process to terminate and free related memory in event of a
 * SIGINT signal.
 *
 * currentWorker: Worker currently executing (SIGINT is only caught for
 *                sequential execution so we know there will only be one
 *                running at a time).
 * comp: Compressed object worker is working on.
 * output: Name of output file worker is writing to (archive file for
 *         compression or original file for decompression)
 */
void sig_int_clean_up(Worker* currentWorker, Compressed* comp, char* output)
{
    wait(NULL);
    fprintf(stderr, "uqzip: Execution aborted\n");
    free_worker(currentWorker);
    free_compressed(comp);
    remove(output);
}

/* compress_sequential()
 * ---------------------
 * Compress a set of input files sequentially (Compress one file at a time).
 *
 * param: Command line parameters
 *
 * Returns: 0 if compression was successfull, or the reason for child process
 *          death (SIGNAL_ERROR or COMMAND_ERROR) if child process does not
 *          exit successfully.
 */
int compress_sequential(Parameters* param)
{
    for (int i = 0; i < param->inputCount; i++) {
        int pipefds[2];
        pipe(pipefds);
        // initialise and start a compression worker
        Worker* work = init_worker(param, param->inputFiles[i]);

        if (!(work->pid = fork())) {
            start_worker(work, param->method, pipefds);
        }
        // In parent so close the write end of pipe and empty the data which
        // child process sent over pipe
        close(pipefds[1]);
        Compressed* comp = empty_pipe(pipefds[0]);
        close(pipefds[0]);

        if (!reap_worker(work, param->method)) {
            // worker didn't exit with exit status 0 so free memory and return
            // the worker's reason
            int reason = work->state;
            free_compressed(comp);
            free_worker(work);
            remove(param->outName);
            return reason;
        }
        if (sigIntCaught && (i != param->inputCount - 1)) {
            // interrupted by SIGINT so free memory and wait for current worker
            // to finish what its doing
            sig_int_clean_up(work, comp, param->outName);
            return INTERRUPT_ERROR;
        }
        enter_record(comp, param, i);
        free_worker(work);
        free_compressed(comp);
    }
    return 0;
}

/* compress_parallel()
 * -------------------
 * Compress a set of input files in parallel (Files compressed simultaneously).
 *
 * param: Command line parameters.
 *
 * Returns: 0 if compression was successfull, or the reason for child process
 *          death (SIGNAL_ERROR or COMMAND_ERROR) if child process does not
 *          exit successfully.
 */
int compress_parallel(Parameters* param)
{
    int inputCount = param->inputCount;
    int pipefds[inputCount][2];
    Worker** workers = malloc(inputCount * sizeof(Worker*));

    // executing in parallel so first create and start all compression workers
    for (int i = 0; i < inputCount; i++) {
        pipe(pipefds[i]);
        workers[i] = init_worker(param, param->inputFiles[i]);

        if (!(workers[i]->pid = fork())) {
            // close the file descriptors to other children inherited by this
            // child
            for (int j = 0; j < i; j++) {
                close(pipefds[j][0]);
                close(pipefds[j][1]);
            }
            start_worker(workers[i], param->method, pipefds[i]);
        }
        // Still in parent so close the write end of pipe
        close(pipefds[i][1]);
    }
    for (int j = 0; j < inputCount; j++) {
        Compressed* comp = empty_pipe(pipefds[j][0]);
        close(pipefds[j][0]);

        if (!reap_worker(workers[j], param->method)) {
            // one of the workers didn't exit correctly so send remaining
            // worker SIGTERM to terminate them
            int reason = workers[j]->state;
            signal_workers(workers, j, inputCount, SIGTERM);
            free_compressed(comp);
            remove(param->outName);
            free_workers(workers, inputCount);
            return reason;
        }
        enter_record(comp, param, j);
        free_compressed(comp);
    }
    free_workers(workers, inputCount);
    return EXIT_OK;
}

/* decompress_sequential()
 * -----------------------
 * Decompress a .uqz archive sequentially (One file from the archive is
 * processed at a time).
 *
 * param: Parameters passed through command line
 * header: Header section of the archive file to be decompressed
 *
 * Returns: EXIT_OK if successfull, FORMAT_ERROR if the archive file is not
 *          in a .uqz format or INTERRUPT_ERROR if a SIGINT is sent to uqzip
 *          whilst decompressing.
 */
int decompress_sequential(Parameters* param, UqzHeaderSection* header)
{
    for (uint32_t i = 0; i < header->numFiles; i++) {
        int pipefd[2];
        int verifyRecord;
        pipe(pipefd);

        if ((verifyRecord = verify_extractable(param->archive, header, i))) {
            return verifyRecord;
        }
        FILE* archive = fopen(param->archive, "r");
        Compressed* extract = read_record(archive, header, i);
        // close so children don't inherit
        fclose(archive);

        if (extract->size == 0) {
            // create empty output file if extracted size is 0
            create_empty_file(extract->orgFile);
            free_compressed(extract);
            continue;
        }
        Worker* work = init_worker(param, extract->orgFile);

        if (!(work->pid = fork())) {
            start_worker(work, header->method, pipefd);
        }
        // send compressed data to child through pipe
        close(pipefd[0]);
        write(pipefd[1], extract->data, extract->size);
        close(pipefd[1]);

        if (!reap_worker(work, header->method)) {
            // worker didn't exit correctly
            int reason = work->state;
            free_worker(work);
            remove(extract->orgFile);
            free_compressed(extract);
            return reason;
        }
        if (sigIntCaught && (i != header->numFiles - 1)) {
            sig_int_clean_up(work, extract, extract->orgFile);
            return INTERRUPT_ERROR;
        }
        printf("\"%s\" has been extracted\n", extract->orgFile);
        free_compressed(extract);
        free_worker(work);
    }
    return EXIT_OK;
}

/* find_reaped()
 * -------------
 * Find the worker which was reaped by looking for the matching pid in workers
 * array. Used when decompressing in parallel as workers are reaped at random.
 *
 * workers: Array of worker pairs
 * pid: Pid of worker which was reaped
 * size: Number of pairs in workers array
 *
 * Returns: Index of pair in workers which reaped worker is located
 */
int find_reaped(Worker* (*workers)[2], pid_t pid, int size)
{
    int index = 0;

    while (((workers[index][0]->pid != pid)
            && (workers[index][1]->pid != pid))) {
        if (index == size - 1) {
            break;
        }
        index++;
    }
    return index;
}

/* clean_up_worker_pairs()
 * -----------------------
 * Reap pairs of workers and signal remaining workers accordingly.
 *
 * workers: Array of worker pairs to clean up
 * size: Number of pairs in workers array
 * method: CompMethod being used by workers
 *
 * Returns: EXIT_OK if all pairs exited with exit status 0, otherwise the
 *          reason for death of the worker which failed.
 */
int clean_up_worker_pairs(Worker* (*workers)[2], int size, CompMethod method)
{
    // loop for 2 * size since size is number of worker pairs and therefore
    // there are 2 * size children to be reaped
    for (int i = 0; i < PARALLEL_DECOMP_PAIR_SIZE * size; i++) {
        int status;
        pid_t pid = wait(&status);
        int failIndex = find_reaped(workers, pid, size);
        Worker* reaped;

        if (workers[failIndex][0]->pid == pid) {
            reaped = workers[failIndex][0];
        } else {
            reaped = workers[failIndex][1];
        }
        // only enter here for workers[x][0] since workers[x][1] will always
        // exit with EXIT_OK
        if (!get_exit_reason(reaped, status, method)) {
            // check if partner to worker is also still running and needs to be
            // terminated
            if (workers[failIndex][1]->state == WORKER_RUNNING) {
                kill(workers[failIndex][1]->pid, SIGTERM);
                waitpid(workers[failIndex][1]->pid, NULL, 0);
            }
            remove(workers[failIndex][0]->workingOn);

            // signal remaining running pairs with SIGTERM and remove the output
            // file they were working on
            signal_and_remove_worker_pairs(workers, size);
            return workers[failIndex][0]->state;
        }
    }
    return EXIT_OK;
}

/* decompress_parallel()
 * ---------------------
 * Decompress a .uqz archive in parallel (One process for each file in the
 * archive).
 *
 * param: Parameters passed through command line
 * header: Header section of the archive file
 *
 * Returns: EXIT_OK if archive was successfully decompressed, FORMAT_ERROR if
 *          the archive file is not in a .uqz format or, if a uqzip child
 *          process exits abnormally, the exit reason for that child process
 */
int decompress_parallel(Parameters* param, UqzHeaderSection* header)
{
    // workers[x][0] will be child with read end of pipe and workers[x][1]
    // will be child with write end of pipe
    int inputCount = header->numFiles;
    int pipefds[inputCount][PARALLEL_DECOMP_PAIR_SIZE];
    int activePairs = 0;
    Worker* workers[inputCount][PARALLEL_DECOMP_PAIR_SIZE];

    for (int i = 0; i < inputCount; i++) {
        int verify;

        if ((verify = verify_extractable(param->archive, header, i))) {
            // can't extract record so signal active pairs
            signal_and_remove_worker_pairs(workers, activePairs);
            free_worker_pairs(workers, activePairs);
            return verify;
        }
        FILE* archive = fopen(param->archive, "r");
        Compressed* extract = read_record(archive, header, i);
        // close so children dont inherit
        fclose(archive);

        if (extract->size == 0) {
            free_compressed(extract);
            create_empty_file(extract->orgFile);
            continue;
        }
        pipe(pipefds[i]);
        // initialise and start worker pair
        workers[i][0] = init_worker(param, extract->orgFile);
        workers[i][1] = init_worker(param, extract->orgFile);
        start_worker_pair(workers[i], extract, param->method, pipefds[i]);
        activePairs++;

        if ((workers[i][0]->pid) && (workers[i][1]->pid)) {
            // still in parent so close both ends of pipe (parent wont
            // communicate with children)
            close(pipefds[i][0]);
            close(pipefds[i][1]);
        }
        free_compressed(extract);
    }
    int pairStatus = clean_up_worker_pairs(workers, inputCount, param->method);
    free_worker_pairs(workers, activePairs);

    if (pairStatus != EXIT_OK) {
        return pairStatus;
    }
    return EXIT_OK;
}

/* decompress_archive()
 * --------------------
 * Decompress a .uqz archive file.
 *
 * param: Parameters passed through command line
 *
 * Returns: READ_ERROR if archive file cannot be opened for reading,
 *          FORMAT_ERROR if the archive file does not contain a valid header
 *          section.
 */
int decompress_archive(Parameters* param)
{
    FILE* archive = fopen(param->archive, "r");

    if (archive == NULL) {
        fprintf(stderr, "uqzip: can't open file \"%s\" for reading\n",
                param->archive);
        return READ_ERROR;
    }
    UqzHeaderSection* header = read_uqz_header_section(archive);

    // invalid or incorrectly formatted header
    if (header == NULL) {
        fprintf(stderr, "uqzip: File \"%s\" has invalid format\n",
                param->archive);
        return FORMAT_ERROR;
    }
    // update param with method which was originally used to create archive
    param->method = header->method;
    fclose(archive);
    int status = EXIT_OK;

    if (param->parallel) {
        status = decompress_parallel(param, header);
    } else {
        status = decompress_sequential(param, header);
    }
    free_uqz_header_section(header);
    return status;
}

/* compress_files()
 * ----------------
 * Compress a set of files into a .uqz archive.
 *
 * param: Parameters passed through command line
 *
 * Returns: WRITE_ERROR if output file cannot be opened for writing otherwise
 *          returns the status of compress_parallel() if compressing in
 *          parallel or compress_sequential() if compressing sequentially.
 */
int compress_files(Parameters* param)
{
    int status;

    if (!write_header_section(param)) {
        // couldn't write the header
        return WRITE_ERROR;
    }

    if (param->parallel) {
        status = compress_parallel(param);
    } else {
        status = compress_sequential(param);
    }
    return status;
}

/* sigint_handler()
 * ----------------
 * Callback function for a SIGINT signal. Sets global variable sigIntCaught
 * to true.
 *
 * signal: Type of signal (unused since only being called for SIGINT)
 */
void sigint_handler(int signal __attribute__((unused)))
{
    sigIntCaught = true;
}

int main(int argc, char** argv)
{
    Parameters* param = process_cmdline_args(argc, argv);
    // setup the handler for SIGINT using sigaction
    struct sigaction sigAct;
    memset(&sigAct, 0, sizeof(sigAct));
    sigAct.sa_handler = sigint_handler;
    sigAct.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sigAct, 0);

    if (param == NULL) {
        fprintf(stderr,
                "Usage: ./uqzip [--output outputFileName] [--parallel] "
                "[--nocomp|--gz|--zip|--xz|--bzip2] filename ...\n");
        fprintf(stderr,
                "   Or: ./uqzip --decompress [--parallel] archive-file\n");
        exit(USAGE_ERROR);
    }
    if (param->method != DECOMP) {
        if (!write_header_section(param)) {
            // couldn't write header section of archive
            free_parameters(param);
            exit(WRITE_ERROR);
        }
        int compStatus = compress_files(param);

        if (compStatus) {
            // something went wrong with compression
            free_parameters(param);
            exit(compStatus);
        }
    } else {
        int deCompStatus = decompress_archive(param);

        if (deCompStatus) {
            // something went wrong with decompression
            free_parameters(param);
            exit(deCompStatus);
        }
    }
    free_parameters(param);
    exit(EXIT_OK);
}
