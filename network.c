#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETWORK_MAGIC "DPCMDN01"
#define NETWORK_MAGIC_SIZE 8
#define NETWORK_MAX_ARGS 128
#define NETWORK_MAX_ARG_SIZE (64U * 1024U)
#define NETWORK_MAX_FILE_SIZE (1024ULL * 1024ULL * 1024ULL)
#define NETWORK_NO_FILE UINT32_MAX
#define NETWORK_OUTPUT_FRAME 'O'
#define NETWORK_EXIT_FRAME 'X'

struct command_request {
    uint32_t argc;
    char** argv;
    uint32_t file_index;
    uint64_t file_size;
};

static int write_all(int fd, const void* data, size_t size)
{
    const unsigned char* p = data;

    while (size != 0) {
        ssize_t written = write(fd, p, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            return -1;
        p += written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void* data, size_t size)
{
    unsigned char* p = data;

    while (size != 0) {
        ssize_t received = read(fd, p, size);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (received == 0)
            return -1;
        p += received;
        size -= (size_t)received;
    }
    return 0;
}

static int send_u32(int fd, uint32_t value)
{
    value = htonl(value);
    return write_all(fd, &value, sizeof(value));
}

static int receive_u32(int fd, uint32_t* value)
{
    uint32_t wire_value;

    if (read_all(fd, &wire_value, sizeof(wire_value)) < 0)
        return -1;
    *value = ntohl(wire_value);
    return 0;
}

static int send_u64(int fd, uint64_t value)
{
    uint32_t words[2];

    words[0] = htonl((uint32_t)(value >> 32));
    words[1] = htonl((uint32_t)value);
    return write_all(fd, words, sizeof(words));
}

static int receive_u64(int fd, uint64_t* value)
{
    uint32_t words[2];

    if (read_all(fd, words, sizeof(words)) < 0)
        return -1;
    *value = ((uint64_t)ntohl(words[0]) << 32) | ntohl(words[1]);
    return 0;
}

static int send_frame(int fd, unsigned char type, const void* data, uint32_t size)
{
    if (write_all(fd, &type, 1) < 0 || send_u32(fd, size) < 0)
        return -1;
    return size == 0 ? 0 : write_all(fd, data, size);
}

static void send_error(int fd, const char* message)
{
    uint32_t status = htonl(2);

    send_frame(fd, NETWORK_OUTPUT_FRAME, message, (uint32_t)strlen(message));
    send_frame(fd, NETWORK_EXIT_FRAME, &status, sizeof(status));
}

static int parse_endpoint(const char* text, int listen_mode, char** host, char** port)
{
    const char* colon;
    const char* port_start;
    size_t host_len;
    char* end;
    long number;

    *host = NULL;
    *port = NULL;
    if (text == NULL || *text == '\0')
        return -1;

    if (text[0] == '[') {
        const char* close = strchr(text, ']');
        if (close == NULL || close[1] != ':' || close[2] == '\0')
            return -1;
        host_len = (size_t)(close - text - 1);
        *host = strndup(text + 1, host_len);
        port_start = close + 2;
    } else {
        colon = strrchr(text, ':');
        if (colon == NULL) {
            if (!listen_mode)
                return -1;
            *host = strdup("127.0.0.1");
            port_start = text;
        } else {
            host_len = (size_t)(colon - text);
            if (host_len == 0)
                *host = listen_mode ? NULL : strdup("");
            else
                *host = strndup(text, host_len);
            port_start = colon + 1;
        }
    }

    errno = 0;
    number = strtol(port_start, &end, 10);
    if (errno != 0 || *port_start == '\0' || *end != '\0' || number < 1 || number > 65535) {
        free(*host);
        *host = NULL;
        return -1;
    }
    *port = strdup(port_start);
    if (*port == NULL || (listen_mode == 0 && (*host == NULL || **host == '\0'))) {
        free(*host);
        free(*port);
        *host = NULL;
        *port = NULL;
        return -1;
    }
    return 0;
}

static int connect_endpoint(const char* endpoint)
{
    struct addrinfo hints;
    struct addrinfo* addresses;
    struct addrinfo* address;
    char* host;
    char* port;
    int fd = -1;
    int error;

    if (parse_endpoint(endpoint, 0, &host, &port) < 0) {
        fprintf(stderr, "Error: invalid server address '%s' (expected HOST:PORT).\n", endpoint);
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Error: cannot resolve server '%s': %s.\n", endpoint, gai_strerror(error));
        free(host);
        free(port);
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    if (fd < 0)
        fprintf(stderr, "Error: cannot connect to %s: %s.\n", endpoint, strerror(errno));
    freeaddrinfo(addresses);
    free(host);
    free(port);
    return fd;
}

static int create_listener(const char* endpoint)
{
    struct addrinfo hints;
    struct addrinfo* addresses;
    struct addrinfo* address;
    char* host;
    char* port;
    int fd = -1;
    int enabled = 1;
    int error;

    if (parse_endpoint(endpoint, 1, &host, &port) < 0) {
        fprintf(stderr, "Error: invalid listen address '%s' (expected [ADDRESS:]PORT).\n", endpoint);
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Error: cannot resolve listen address '%s': %s.\n", endpoint, gai_strerror(error));
        free(host);
        free(port);
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, 4) == 0)
            break;
        close(fd);
        fd = -1;
    }
    if (fd < 0)
        fprintf(stderr, "Error: cannot listen on %s: %s.\n", endpoint, strerror(errno));
    freeaddrinfo(addresses);
    free(host);
    free(port);
    return fd;
}

static int is_long_option(const char* arg, const char* name)
{
    size_t length = strlen(name);
    return strcmp(arg, name) == 0 || (strncmp(arg, name, length) == 0 && arg[length] == '=');
}

static int is_long_abbreviation(const char* arg, const char* name)
{
    const char* equal;
    size_t length;

    if (strncmp(arg, "--", 2) != 0)
        return 0;
    equal = strchr(arg, '=');
    length = equal == NULL ? strlen(arg) : (size_t)(equal - arg);
    return length < strlen(name) && length > 2 && strncmp(arg, name, length) == 0;
}

static int forbidden_option(const char* arg)
{
    static const char* const long_options[] = {
        "--listen", "--server", "--read", "--raw-instruction",
        "--raw-require-return", "--update-fw", "--log"
    };
    static const char short_options[] = { 'r', 'I', 'R', 'U', 'O' };
    size_t i;

    for (i = 0; i < sizeof(long_options) / sizeof(long_options[0]); ++i) {
        if (is_long_option(arg, long_options[i]))
            return 1;
    }
    if (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0') {
        for (i = 0; i < sizeof(short_options); ++i) {
            if (arg[1] == short_options[i])
                return 1;
        }
    }
    return 0;
}

static const char* input_long_name(const char* arg)
{
    static const char* const names[] = {
        "--prog", "--auto", "--batch", "--fsum", "--loadFile-with-verify"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (is_long_option(arg, names[i]))
            return names[i];
    }
    return NULL;
}

static int input_short_option(char option)
{
    return option == 'p' || option == 'u' || option == 'z' || option == 'f' || option == 'W';
}

static int short_option_requires_argument(char option)
{
    return strchr("rpuzfIRalxTSNBtgcOk14UE", option) != NULL;
}

/* Return -1 for a forbidden option, its input-option offset, or zero. */
static int analyze_short_options(const char* arg)
{
    size_t i;

    if (arg[0] != '-' || arg[1] == '-' || arg[1] == '\0')
        return 0;
    for (i = 1; arg[i] != '\0'; ++i) {
        if (strchr("rIRUO", arg[i]) != NULL)
            return -1;
        if (input_short_option(arg[i]))
            return (int)i;
        if (short_option_requires_argument(arg[i]))
            return 0;
    }
    return 0;
}

static int unsafe_long_abbreviation(const char* arg)
{
    static const char* const path_options[] = {
        "--listen", "--server", "--read", "--raw-instruction",
        "--raw-require-return", "--update-fw", "--log", "--prog", "--auto",
        "--batch", "--fsum", "--loadFile-with-verify"
    };
    size_t i;

    for (i = 0; i < sizeof(path_options) / sizeof(path_options[0]); ++i) {
        if (is_long_abbreviation(arg, path_options[i]))
            return 1;
    }
    return 0;
}

static void free_request(struct command_request* request)
{
    uint32_t i;

    if (request->argv != NULL) {
        for (i = 0; i < request->argc; ++i)
            free(request->argv[i]);
        free(request->argv);
    }
    memset(request, 0, sizeof(*request));
    request->file_index = NETWORK_NO_FILE;
}

static int append_argument(struct command_request* request, const char* value)
{
    if (request->argc >= NETWORK_MAX_ARGS)
        return -1;
    request->argv[request->argc] = strdup(value);
    if (request->argv[request->argc] == NULL)
        return -1;
    ++request->argc;
    request->argv[request->argc] = NULL;
    return 0;
}

static int build_client_request(int argc, char* argv[], int server_index,
    struct command_request* request, const char** filename)
{
    int i;

    memset(request, 0, sizeof(*request));
    request->file_index = NETWORK_NO_FILE;
    request->argv = calloc(NETWORK_MAX_ARGS + 1, sizeof(char*));
    if (request->argv == NULL)
        return -1;
    if (append_argument(request, argv[0]) < 0)
        goto invalid;

    for (i = 1; i < argc; ++i) {
        const char* long_name;
        const char* value;
        int short_input;

        if (i == server_index) {
            if (strcmp(argv[i], "--server") == 0)
                ++i;
            continue;
        }
        short_input = analyze_short_options(argv[i]);
        if (forbidden_option(argv[i]) || short_input < 0 || unsafe_long_abbreviation(argv[i])) {
            fprintf(stderr, "Error: option '%s' is not allowed in network mode.\n", argv[i]);
            goto invalid;
        }
        long_name = input_long_name(argv[i]);
        if (long_name != NULL) {
            const char* equal = strchr(argv[i], '=');
            if (request->file_index != NETWORK_NO_FILE) {
                fprintf(stderr, "Error: network mode accepts only one input image.\n");
                goto invalid;
            }
            if (equal != NULL)
                value = equal + 1;
            else if (++i < argc)
                value = argv[i];
            else {
                fprintf(stderr, "Error: option '%s' requires a file.\n", long_name);
                goto invalid;
            }
            if (*value == '\0' || strchr(value, '|') != NULL) {
                fprintf(stderr, "Error: network mode requires one non-empty BIN image path.\n");
                goto invalid;
            }
            if (append_argument(request, long_name) < 0 || append_argument(request, value) < 0)
                goto invalid;
            request->file_index = request->argc - 1;
            *filename = value;
            continue;
        }
        if (short_input > 0) {
            char option[3] = { '-', argv[i][short_input], '\0' };
            int prefix;
            if (request->file_index != NETWORK_NO_FILE) {
                fprintf(stderr, "Error: network mode accepts only one input image.\n");
                goto invalid;
            }
            for (prefix = 1; prefix < short_input; ++prefix) {
                char flag[3] = { '-', argv[i][prefix], '\0' };
                if (append_argument(request, flag) < 0)
                    goto invalid;
            }
            if (argv[i][short_input + 1] != '\0')
                value = argv[i] + short_input + 1;
            else if (++i < argc)
                value = argv[i];
            else {
                fprintf(stderr, "Error: option '%s' requires a file.\n", option);
                goto invalid;
            }
            if (*value == '\0' || strchr(value, '|') != NULL) {
                fprintf(stderr, "Error: network mode requires one non-empty BIN image path.\n");
                goto invalid;
            }
            if (append_argument(request, option) < 0 || append_argument(request, value) < 0)
                goto invalid;
            request->file_index = request->argc - 1;
            *filename = value;
            continue;
        }
        if (append_argument(request, argv[i]) < 0)
            goto invalid;
    }
    return 0;

invalid:
    free_request(request);
    return -1;
}

static int send_request(int fd, const struct command_request* request, int image_fd)
{
    unsigned char buffer[64 * 1024];
    uint64_t remaining = request->file_size;
    uint32_t i;

    if (write_all(fd, NETWORK_MAGIC, NETWORK_MAGIC_SIZE) < 0 || send_u32(fd, request->argc) < 0)
        return -1;
    for (i = 0; i < request->argc; ++i) {
        size_t length = strlen(request->argv[i]);
        if (send_u32(fd, (uint32_t)length) < 0 || write_all(fd, request->argv[i], length) < 0)
            return -1;
    }
    if (send_u32(fd, request->file_index) < 0 || send_u64(fd, request->file_size) < 0)
        return -1;
    while (remaining != 0) {
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t count = read(image_fd, buffer, wanted);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || write_all(fd, buffer, (size_t)count) < 0)
            return -1;
        remaining -= (uint64_t)count;
    }
    return 0;
}

static int receive_response(int fd)
{
    unsigned char type;
    unsigned char buffer[64 * 1024];

    for (;;) {
        uint32_t size;
        uint32_t remaining;

        if (read_all(fd, &type, 1) < 0 || receive_u32(fd, &size) < 0) {
            fprintf(stderr, "Error: connection closed before the server returned a result.\n");
            return 2;
        }
        if (type == NETWORK_EXIT_FRAME) {
            uint32_t status;
            if (size != sizeof(status) || read_all(fd, &status, sizeof(status)) < 0) {
                fprintf(stderr, "Error: invalid response from server.\n");
                return 2;
            }
            return (int)ntohl(status);
        }
        if (type != NETWORK_OUTPUT_FRAME || size > NETWORK_MAX_ARG_SIZE) {
            fprintf(stderr, "Error: invalid response from server.\n");
            return 2;
        }
        remaining = size;
        while (remaining != 0) {
            uint32_t chunk = remaining < sizeof(buffer) ? remaining : (uint32_t)sizeof(buffer);
            if (read_all(fd, buffer, chunk) < 0 || write_all(STDOUT_FILENO, buffer, chunk) < 0)
                return 2;
            remaining -= chunk;
        }
    }
}

static int run_client(int argc, char* argv[], int server_index, const char* endpoint)
{
    struct command_request request;
    struct stat info;
    const char* filename = NULL;
    int image_fd = -1;
    int fd;
    int result = 2;

    if (build_client_request(argc, argv, server_index, &request, &filename) < 0)
        return 2;
    if (filename != NULL) {
        image_fd = open(filename, O_RDONLY);
        if (image_fd < 0 || fstat(image_fd, &info) < 0 || !S_ISREG(info.st_mode)) {
            fprintf(stderr, "Error: cannot open regular input image '%s': %s.\n", filename, strerror(errno));
            goto done;
        }
        request.file_size = (uint64_t)info.st_size;
        if (request.file_size > NETWORK_MAX_FILE_SIZE) {
            fprintf(stderr, "Error: input image is larger than the 1 GiB network limit.\n");
            goto done;
        }
    }
    fd = connect_endpoint(endpoint);
    if (fd < 0)
        goto done;
    if (send_request(fd, &request, image_fd) < 0)
        fprintf(stderr, "Error: failed to send command to server: %s.\n", strerror(errno));
    else
        result = receive_response(fd);
    close(fd);

done:
    if (image_fd >= 0)
        close(image_fd);
    free_request(&request);
    return result;
}

static int receive_request(int fd, struct command_request* request, char* temp_path)
{
    char magic[NETWORK_MAGIC_SIZE];
    uint32_t i;
    int temp_fd = -1;

    memset(request, 0, sizeof(*request));
    request->file_index = NETWORK_NO_FILE;
    if (read_all(fd, magic, sizeof(magic)) < 0 || memcmp(magic, NETWORK_MAGIC, sizeof(magic)) != 0
        || receive_u32(fd, &request->argc) < 0 || request->argc == 0 || request->argc > NETWORK_MAX_ARGS)
        return -1;
    request->argv = calloc(request->argc + 1, sizeof(char*));
    if (request->argv == NULL)
        return -1;
    for (i = 0; i < request->argc; ++i) {
        uint32_t length;
        if (receive_u32(fd, &length) < 0 || length == 0 || length > NETWORK_MAX_ARG_SIZE)
            goto invalid;
        request->argv[i] = malloc((size_t)length + 1);
        if (request->argv[i] == NULL || read_all(fd, request->argv[i], length) < 0)
            goto invalid;
        request->argv[i][length] = '\0';
    }
    if (receive_u32(fd, &request->file_index) < 0 || receive_u64(fd, &request->file_size) < 0
        || request->file_size > NETWORK_MAX_FILE_SIZE)
        goto invalid;
    if ((request->file_index == NETWORK_NO_FILE) != (request->file_size == 0)) {
        /* Empty files are valid, so NO_FILE is the authoritative value. */
        if (request->file_index == NETWORK_NO_FILE && request->file_size != 0)
            goto invalid;
    }
    if (request->file_index != NETWORK_NO_FILE) {
        unsigned char buffer[64 * 1024];
        uint64_t remaining = request->file_size;

        if (request->file_index == 0 || request->file_index >= request->argc)
            goto invalid;
        strcpy(temp_path, "/tmp/dpcmd-network-XXXXXX");
        temp_fd = mkstemp(temp_path);
        if (temp_fd < 0)
            goto invalid;
        while (remaining != 0) {
            size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            if (read_all(fd, buffer, chunk) < 0 || write_all(temp_fd, buffer, chunk) < 0)
                goto invalid;
            remaining -= chunk;
        }
        if (close(temp_fd) < 0)
            goto invalid;
        temp_fd = -1;
        free(request->argv[request->file_index]);
        request->argv[request->file_index] = strdup(temp_path);
        if (request->argv[request->file_index] == NULL)
            goto invalid;
    }
    return 0;

invalid:
    if (temp_fd >= 0)
        close(temp_fd);
    if (*temp_path != '\0')
        unlink(temp_path);
    free_request(request);
    return -1;
}

static int validate_server_request(const struct command_request* request)
{
    uint32_t i;
    uint32_t found_file = NETWORK_NO_FILE;

    for (i = 1; i < request->argc; ++i) {
        int short_input = analyze_short_options(request->argv[i]);
        if (forbidden_option(request->argv[i]) || short_input < 0
            || unsafe_long_abbreviation(request->argv[i])
            || (short_input > 0 && request->argv[i][2] != '\0')
            || (input_long_name(request->argv[i]) != NULL
                && strchr(request->argv[i], '=') != NULL))
            return -1;
        if ((strcmp(request->argv[i], "-p") == 0 || strcmp(request->argv[i], "-u") == 0
                || strcmp(request->argv[i], "-z") == 0 || strcmp(request->argv[i], "-f") == 0
                || strcmp(request->argv[i], "-W") == 0 || strcmp(request->argv[i], "--prog") == 0
                || strcmp(request->argv[i], "--auto") == 0 || strcmp(request->argv[i], "--batch") == 0
                || strcmp(request->argv[i], "--fsum") == 0
                || strcmp(request->argv[i], "--loadFile-with-verify") == 0)) {
            if (++i >= request->argc || found_file != NETWORK_NO_FILE)
                return -1;
            found_file = i;
        }
    }
    return found_file == request->file_index ? 0 : -1;
}

static int executable_path(const char* fallback, char* path, size_t size)
{
#ifdef __linux__
    ssize_t length = readlink("/proc/self/exe", path, size - 1);
    if (length > 0 && (size_t)length < size) {
        path[length] = '\0';
        return 0;
    }
#endif
    if (strlen(fallback) >= size)
        return -1;
    strcpy(path, fallback);
    return 0;
}

static void serve_connection(int fd, const char* program)
{
    struct command_request request;
    char temp_path[] = "/tmp/dpcmd-network-XXXXXX";
    char program_path[4096];
    unsigned char buffer[16 * 1024];
    int output_pipe[2];
    int status;
    pid_t child;

    temp_path[0] = '\0';
    if (receive_request(fd, &request, temp_path) < 0) {
        send_error(fd, "Error: invalid network request.\n");
        return;
    }
    if (validate_server_request(&request) < 0 || executable_path(program, program_path, sizeof(program_path)) < 0) {
        send_error(fd, "Error: command is not allowed in network mode.\n");
        goto done;
    }
    free(request.argv[0]);
    request.argv[0] = strdup(program_path);
    if (request.argv[0] == NULL || pipe(output_pipe) < 0) {
        send_error(fd, "Error: server could not start dpcmd.\n");
        goto done;
    }
    child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        send_error(fd, "Error: server could not start dpcmd.\n");
        goto done;
    }
    if (child == 0) {
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[1]);
        close(fd);
        execv(program_path, request.argv);
        _exit(127);
    }
    close(output_pipe[1]);
    for (;;) {
        ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        if (send_frame(fd, NETWORK_OUTPUT_FRAME, buffer, (uint32_t)count) < 0)
            break;
    }
    close(output_pipe[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))
        status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        status = 128 + WTERMSIG(status);
    else
        status = 2;
    {
        uint32_t wire_status = htonl((uint32_t)status);
        send_frame(fd, NETWORK_EXIT_FRAME, &wire_status, sizeof(wire_status));
    }

done:
    if (temp_path[0] != '\0')
        unlink(temp_path);
    free_request(&request);
}

static int run_server(const char* endpoint, const char* program)
{
    int listener = create_listener(endpoint);

    if (listener < 0)
        return 2;
    printf("Listening for dpcmd clients on %s\n", endpoint);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Error: accept failed: %s.\n", strerror(errno));
            close(listener);
            return 2;
        }
        serve_connection(fd, program);
        close(fd);
    }
}

int NetworkHandleMode(int argc, char* argv[])
{
    const char* listen_endpoint = NULL;
    const char* server_endpoint = NULL;
    int server_index = -1;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--listen") == 0 || strcmp(argv[i], "--server") == 0) {
            int is_server = strcmp(argv[i], "--server") == 0;
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an address.\n", argv[i]);
                return 2;
            }
            if (is_server) {
                if (server_endpoint != NULL) {
                    fprintf(stderr, "Error: --server may only be specified once.\n");
                    return 2;
                }
                server_endpoint = argv[i + 1];
                server_index = i;
            } else {
                if (listen_endpoint != NULL) {
                    fprintf(stderr, "Error: --listen may only be specified once.\n");
                    return 2;
                }
                listen_endpoint = argv[i + 1];
            }
            ++i;
        } else if (strncmp(argv[i], "--listen=", 9) == 0) {
            if (listen_endpoint != NULL) {
                fprintf(stderr, "Error: --listen may only be specified once.\n");
                return 2;
            }
            listen_endpoint = argv[i] + 9;
        } else if (strncmp(argv[i], "--server=", 9) == 0) {
            if (server_endpoint != NULL) {
                fprintf(stderr, "Error: --server may only be specified once.\n");
                return 2;
            }
            server_endpoint = argv[i] + 9;
            server_index = i;
        }
    }
    if (listen_endpoint == NULL && server_endpoint == NULL)
        return NETWORK_MODE_NOT_REQUESTED;
    if (listen_endpoint != NULL && server_endpoint != NULL) {
        fprintf(stderr, "Error: --listen and --server cannot be used together.\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (listen_endpoint != NULL) {
        if (argc != 3 && !(argc == 2 && strncmp(argv[1], "--listen=", 9) == 0)) {
            fprintf(stderr, "Error: --listen cannot be combined with a dpcmd operation.\n");
            return 2;
        }
        return run_server(listen_endpoint, argv[0]);
    }
    return run_client(argc, argv, server_index, server_endpoint);
}
