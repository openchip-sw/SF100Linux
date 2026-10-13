#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETWORK_MAGIC 0x4450434eU /* "DPCN" */
#define NETWORK_VERSION 1U
#define NETWORK_MAX_ARGS 128U
#define NETWORK_MAX_ARG_LENGTH 4096U
#define NETWORK_MAX_IMAGE_SIZE (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define NETWORK_IO_SIZE 65536U
#define NETWORK_NO_IMAGE UINT32_MAX

struct NetworkRequest {
    uint32_t argc;
    uint32_t image_index;
    uint64_t image_size;
    char** argv;
};

static uint64_t host_to_network64(uint64_t value)
{
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)value);
    return ((uint64_t)low << 32) | high;
}

static uint64_t network_to_host64(uint64_t value)
{
    uint32_t high = ntohl((uint32_t)value);
    uint32_t low = ntohl((uint32_t)(value >> 32));
    return ((uint64_t)high << 32) | low;
}

static int send_all(int fd, const void* data, size_t length)
{
    const unsigned char* cursor = data;

    while (length > 0) {
        ssize_t sent = send(fd, cursor, length, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        cursor += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int receive_all(int fd, void* data, size_t length)
{
    unsigned char* cursor = data;

    while (length > 0) {
        ssize_t received = recv(fd, cursor, length, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return -1;
        cursor += received;
        length -= (size_t)received;
    }
    return 0;
}

static int send_u32(int fd, uint32_t value)
{
    value = htonl(value);
    return send_all(fd, &value, sizeof(value));
}

static int receive_u32(int fd, uint32_t* value)
{
    if (receive_all(fd, value, sizeof(*value)) != 0)
        return -1;
    *value = ntohl(*value);
    return 0;
}

static int split_endpoint(const char* endpoint, int listener, char* host, size_t host_size, char* port, size_t port_size)
{
    const char* separator = strrchr(endpoint, ':');
    const char* port_value;
    size_t host_length;

    if (separator == NULL) {
        if (!listener) {
            fprintf(stderr, "Error: --server expects HOST:PORT.\n");
            return -1;
        }
        strncpy(host, "127.0.0.1", host_size - 1);
        host[host_size - 1] = '\0';
        port_value = endpoint;
    } else {
        host_length = (size_t)(separator - endpoint);
        if (host_length == 0 || host_length >= host_size) {
            fprintf(stderr, "Error: invalid network address '%s'.\n", endpoint);
            return -1;
        }
        memcpy(host, endpoint, host_length);
        host[host_length] = '\0';
        port_value = separator + 1;
    }

    if (*port_value == '\0' || strlen(port_value) >= port_size) {
        fprintf(stderr, "Error: invalid network port in '%s'.\n", endpoint);
        return -1;
    }
    strncpy(port, port_value, port_size - 1);
    port[port_size - 1] = '\0';
    return 0;
}

static int open_connection(const char* endpoint)
{
    char host[256] = { 0 };
    char port[16] = { 0 };
    struct addrinfo hints;
    struct addrinfo* addresses = NULL;
    struct addrinfo* address;
    int fd = -1;
    int error;

    if (split_endpoint(endpoint, 0, host, sizeof(host), port, sizeof(port)) != 0)
        return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Error: cannot resolve %s: %s.\n", host, gai_strerror(error));
        return -1;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        fprintf(stderr, "Error: cannot connect to %s.\n", endpoint);
    return fd;
}

static int open_listener(const char* endpoint)
{
    char host[256] = { 0 };
    char port[16] = { 0 };
    struct addrinfo hints;
    struct addrinfo* addresses = NULL;
    struct addrinfo* address;
    int fd = -1;
    int enabled = 1;
    int error;

    if (split_endpoint(endpoint, 1, host, sizeof(host), port, sizeof(port)) != 0)
        return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Error: cannot resolve listen address %s: %s.\n", host, gai_strerror(error));
        return -1;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, 1) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        fprintf(stderr, "Error: cannot listen on %s.\n", endpoint);
    else
        printf("Dpcmd network server listening on %s:%s\n", host, port);
    return fd;
}

static const char* image_long_option(const char* argument)
{
    static const char* options[] = {
        "--auto", "--prog", "--batch", "--fsum", "--loadFile-with-verify"
    };
    size_t i;

    for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        size_t length = strlen(options[i]);
        if (strcmp(argument, options[i]) == 0
            || (strncmp(argument, options[i], length) == 0 && argument[length] == '='))
            return options[i];
    }
    return NULL;
}

static int image_short_option(const char* argument)
{
    return argument[0] == '-' && argument[1] != '\0' && strchr("upzfW", argument[1]) != NULL;
}

static int add_request_argument(struct NetworkRequest* request, const char* value)
{
    if (request->argc >= NETWORK_MAX_ARGS) {
        fprintf(stderr, "Error: too many remote command arguments.\n");
        return -1;
    }
    request->argv[request->argc++] = (char*)value;
    return 0;
}

static int prepare_client_request(int argc, char* argv[], int server_index, int server_has_separate_value,
    struct NetworkRequest* request, const char** image_path)
{
    int i;

    memset(request, 0, sizeof(*request));
    request->image_index = NETWORK_NO_IMAGE;
    request->argv = calloc(NETWORK_MAX_ARGS, sizeof(char*));
    if (request->argv == NULL)
        return -1;

    for (i = 1; i < argc; i++) {
        const char* long_option;
        const char* value;

        if (i == server_index) {
            if (server_has_separate_value)
                i++;
            continue;
        }

        long_option = image_long_option(argv[i]);
        if (long_option != NULL) {
            const char* equals = strchr(argv[i], '=');
            if (request->image_index != NETWORK_NO_IMAGE) {
                fprintf(stderr, "Error: only one input image is supported per remote command.\n");
                return -1;
            }
            if (add_request_argument(request, long_option) != 0)
                return -1;
            if (equals != NULL)
                value = equals + 1;
            else if (++i < argc)
                value = argv[i];
            else {
                fprintf(stderr, "Error: %s needs an image file.\n", long_option);
                return -1;
            }
            request->image_index = request->argc;
            *image_path = value;
            if (add_request_argument(request, value) != 0)
                return -1;
            continue;
        }

        if (image_short_option(argv[i])) {
            char* option;
            if (request->image_index != NETWORK_NO_IMAGE) {
                fprintf(stderr, "Error: only one input image is supported per remote command.\n");
                return -1;
            }
            option = malloc(3);
            if (option == NULL)
                return -1;
            option[0] = '-';
            option[1] = argv[i][1];
            option[2] = '\0';
            if (add_request_argument(request, option) != 0)
                return -1;
            if (argv[i][2] != '\0')
                value = argv[i] + 2;
            else if (++i < argc)
                value = argv[i];
            else {
                fprintf(stderr, "Error: %s needs an image file.\n", option);
                return -1;
            }
            request->image_index = request->argc;
            *image_path = value;
            if (add_request_argument(request, value) != 0)
                return -1;
            continue;
        }

        if (add_request_argument(request, argv[i]) != 0)
            return -1;
    }
    return 0;
}

static int send_request(int socket_fd, const struct NetworkRequest* request, int image_fd)
{
    uint64_t image_size = host_to_network64(request->image_size);
    uint64_t remaining = request->image_size;
    unsigned char buffer[NETWORK_IO_SIZE];
    uint32_t i;

    if (send_u32(socket_fd, NETWORK_MAGIC) != 0 || send_u32(socket_fd, NETWORK_VERSION) != 0
        || send_u32(socket_fd, request->argc) != 0 || send_u32(socket_fd, request->image_index) != 0
        || send_all(socket_fd, &image_size, sizeof(image_size)) != 0)
        return -1;

    for (i = 0; i < request->argc; i++) {
        size_t length = strlen(request->argv[i]);
        if (length > NETWORK_MAX_ARG_LENGTH || send_u32(socket_fd, (uint32_t)length) != 0
            || send_all(socket_fd, request->argv[i], length) != 0)
            return -1;
    }

    while (image_fd >= 0 && remaining > 0) {
        size_t part = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        ssize_t count = read(image_fd, buffer, part);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || send_all(socket_fd, buffer, (size_t)count) != 0)
            return -1;
        remaining -= (uint64_t)count;
    }
    return 0;
}

static int receive_client_result(int socket_fd)
{
    unsigned char buffer[NETWORK_IO_SIZE];
    uint32_t length;
    uint32_t status;

    for (;;) {
        if (receive_u32(socket_fd, &length) != 0) {
            fprintf(stderr, "Error: connection lost while receiving server output.\n");
            return 1;
        }
        if (length == 0)
            break;
        while (length > 0) {
            size_t part = length > sizeof(buffer) ? sizeof(buffer) : length;
            if (receive_all(socket_fd, buffer, part) != 0 || fwrite(buffer, 1, part, stdout) != part)
                return 1;
            length -= (uint32_t)part;
        }
    }
    if (receive_u32(socket_fd, &status) != 0)
        return 1;
    return (int)status;
}

static int run_client(const char* endpoint, int argc, char* argv[], int server_index, int server_has_separate_value)
{
    struct NetworkRequest request;
    const char* image_path = NULL;
    off_t image_size;
    int image_fd = -1;
    int socket_fd;
    int result;

    if (prepare_client_request(argc, argv, server_index, server_has_separate_value, &request, &image_path) != 0)
        return 1;
    if (image_path != NULL) {
        image_fd = open(image_path, O_RDONLY);
        if (image_fd < 0) {
            fprintf(stderr, "Error: cannot open image '%s': %s.\n", image_path, strerror(errno));
            return 1;
        }
        image_size = lseek(image_fd, 0, SEEK_END);
        if (image_size < 0 || (uint64_t)image_size > NETWORK_MAX_IMAGE_SIZE || lseek(image_fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, "Error: invalid or oversized image '%s'.\n", image_path);
            close(image_fd);
            return 1;
        }
        request.image_size = (uint64_t)image_size;
    }

    socket_fd = open_connection(endpoint);
    if (socket_fd < 0) {
        if (image_fd >= 0)
            close(image_fd);
        return 1;
    }
    printf("Connected to dpcmd server at %s.\n", endpoint);
    result = send_request(socket_fd, &request, image_fd);
    if (image_fd >= 0)
        close(image_fd);
    if (result != 0) {
        fprintf(stderr, "Error: failed to send the remote command.\n");
        close(socket_fd);
        return 1;
    }
    result = receive_client_result(socket_fd);
    close(socket_fd);
    return result;
}

static void free_request(struct NetworkRequest* request)
{
    uint32_t i;
    if (request->argv != NULL) {
        for (i = 0; i < request->argc; i++)
            free(request->argv[i]);
    }
    free(request->argv);
    memset(request, 0, sizeof(*request));
}

static int receive_request(int socket_fd, struct NetworkRequest* request)
{
    uint32_t magic;
    uint32_t version;
    uint64_t image_size;
    uint32_t i;

    memset(request, 0, sizeof(*request));
    if (receive_u32(socket_fd, &magic) != 0 || receive_u32(socket_fd, &version) != 0
        || receive_u32(socket_fd, &request->argc) != 0 || receive_u32(socket_fd, &request->image_index) != 0
        || receive_all(socket_fd, &image_size, sizeof(image_size)) != 0)
        return -1;
    request->image_size = network_to_host64(image_size);
    if (magic != NETWORK_MAGIC || version != NETWORK_VERSION || request->argc == 0
        || request->argc > NETWORK_MAX_ARGS || request->image_size > NETWORK_MAX_IMAGE_SIZE
        || (request->image_index != NETWORK_NO_IMAGE && request->image_index >= request->argc))
        return -1;

    request->argv = calloc(request->argc, sizeof(char*));
    if (request->argv == NULL)
        return -1;
    for (i = 0; i < request->argc; i++) {
        uint32_t length;
        if (receive_u32(socket_fd, &length) != 0 || length == 0 || length > NETWORK_MAX_ARG_LENGTH)
            return -1;
        request->argv[i] = malloc((size_t)length + 1);
        if (request->argv[i] == NULL || receive_all(socket_fd, request->argv[i], length) != 0)
            return -1;
        request->argv[i][length] = '\0';
    }
    return 0;
}

static int unsupported_remote_argument(const char* argument)
{
    static const char* blocked[] = {
        "--listen", "--server", "--read", "--update-fw", "--raw-instruction",
        "--raw-require-return", "--log", "--sp", "--su"
    };
    size_t i;

    if (argument[0] == '-' && argument[1] != '\0' && strchr("rIUROFM", argument[1]) != NULL)
        return 1;
    for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        size_t length = strlen(blocked[i]);
        if (strcmp(argument, blocked[i]) == 0
            || (strncmp(argument, blocked[i], length) == 0 && argument[length] == '='))
            return 1;
    }
    return 0;
}

static int validate_request(const struct NetworkRequest* request)
{
    uint32_t i;
    int found_image_option = 0;

    for (i = 0; i < request->argc; i++) {
        if (unsupported_remote_argument(request->argv[i]))
            return -1;
        if (image_long_option(request->argv[i]) != NULL
            || (strlen(request->argv[i]) == 2 && image_short_option(request->argv[i]))) {
            if (i + 1 != request->image_index)
                return -1;
            found_image_option = 1;
        }
    }
    if ((request->image_index == NETWORK_NO_IMAGE) != !found_image_option)
        return -1;
    return 0;
}

/* A listener may require one explicit programmer selector on every request. */
struct DeviceSelection {
    const char* option;
    const char* value;
    unsigned long number;
};

static int parse_device_selection(int argc, char* const argv[], uint32_t image_index,
    struct DeviceSelection* selection)
{
    int i;
    memset(selection, 0, sizeof(*selection));
    for (i = 0; i < argc; i++) {
        const char* argument = argv[i];
        const char* equals;
        size_t length;
        const char* option;
        const char* value;
        char* end;
        if ((uint32_t)i == image_index)
            continue;
        /* Do not permit getopt abbreviations or an option terminator to bypass the check. */
        if (strcmp(argument, "--") == 0)
            return -1;
        if (strncmp(argument, "--", 2) != 0)
            continue;
        equals = strchr(argument, '=');
        length = equals ? (size_t)(equals - argument) : strlen(argument);
        if (length == strlen("--device") && strncmp(argument, "--device", length) == 0)
            option = "--device";
        else if (length == strlen("--device-SN") && strncmp(argument, "--device-SN", length) == 0)
            option = "--device-SN";
        else {
            if (length <= strlen("--device-SN") && strncmp(argument, "--device-SN", length) == 0)
                return -1;
            continue;
        }
        if (selection->option != NULL)
            return -1;
        if (equals)
            value = equals + 1;
        else if (++i < argc && (uint32_t)i != image_index)
            value = argv[i];
        else
            return -1;
        if (*value == '\0' || *value == '-')
            return -1;
        selection->option = option;
        selection->value = value;
        if (strcmp(option, "--device") == 0) {
            /* dpcmd parses --device in hexadecimal. Zero means all programmers. */
            errno = 0;
            selection->number = strtoul(value, &end, 16);
            if (errno || *end || selection->number < 1 || selection->number > 16)
                return -1;
        }
    }
    return 0;
}

static int validate_device_selection(const struct NetworkRequest* request,
    const struct DeviceSelection* required)
{
    struct DeviceSelection selected;
    if (required->option == NULL)
        return 0;
    if (parse_device_selection((int)request->argc, request->argv, request->image_index, &selected) != 0
        || selected.option == NULL || strcmp(selected.option, required->option) != 0)
        return -1;
    if (strcmp(required->option, "--device") == 0)
        return selected.number == required->number ? 0 : -1;
    return strcmp(selected.value, required->value) == 0 ? 0 : -1;
}

static int receive_image(int socket_fd, uint64_t image_size, char* path, size_t path_size)
{
    char temporary[] = "/tmp/dpcmd-network-XXXXXX";
    unsigned char buffer[NETWORK_IO_SIZE];
    uint64_t remaining = image_size;
    int fd = mkstemp(temporary);

    if (fd < 0)
        return -1;
    while (remaining > 0) {
        size_t part = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        size_t written = 0;
        if (receive_all(socket_fd, buffer, part) != 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
        while (written < part) {
            ssize_t count = write(fd, buffer + written, part - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                close(fd);
                unlink(temporary);
                return -1;
            }
            written += (size_t)count;
        }
        remaining -= part;
    }
    close(fd);
    if (strlen(temporary) >= path_size) {
        unlink(temporary);
        return -1;
    }
    strcpy(path, temporary);
    return 0;
}

static int send_output_frame(int socket_fd, const void* data, uint32_t length)
{
    if (send_u32(socket_fd, length) != 0)
        return -1;
    return length == 0 ? 0 : send_all(socket_fd, data, length);
}

static void send_error_result(int socket_fd, const char* message)
{
    send_output_frame(socket_fd, message, (uint32_t)strlen(message));
    send_output_frame(socket_fd, NULL, 0);
    send_u32(socket_fd, 1);
}

static int run_local_command(int socket_fd, const char* executable, struct NetworkRequest* request, const char* image_path, const struct DeviceSelection* required)
{
    char executable_path[PATH_MAX];
    char** child_argv;
    unsigned char buffer[NETWORK_IO_SIZE];
    ssize_t executable_length;
    int output_pipe[2];
    pid_t child;
    int status = 1;
    uint32_t i;
    size_t offset = required->option ? 3 : 1;

    executable_length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
    if (executable_length >= 0)
        executable_path[executable_length] = '\0';
    else {
        strncpy(executable_path, executable, sizeof(executable_path) - 1);
        executable_path[sizeof(executable_path) - 1] = '\0';
    }

    child_argv = calloc((size_t)request->argc + offset + 1, sizeof(char*));
    if (child_argv == NULL || pipe(output_pipe) != 0) {
        free(child_argv);
        return -1;
    }
    child_argv[0] = executable_path;
    /* Put the enforced selector first so another option cannot consume it as a value. */
    if (required->option) {
        child_argv[1] = (char*)required->option;
        child_argv[2] = (char*)required->value;
    }
    for (i = 0; i < request->argc; i++)
        child_argv[i + offset] = (i == request->image_index) ? (char*)image_path : request->argv[i];

    child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        free(child_argv);
        return -1;
    }
    if (child == 0) {
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[1]);
        execv(executable_path, child_argv);
        dprintf(STDERR_FILENO, "Error: cannot execute local dpcmd: %s.\n", strerror(errno));
        _exit(127);
    }

    close(output_pipe[1]);
    for (;;) {
        ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        if (send_output_frame(socket_fd, buffer, (uint32_t)count) != 0)
            break;
    }
    close(output_pipe[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    free(child_argv);

    if (WIFEXITED(status))
        status = WEXITSTATUS(status);
    else
        status = 1;
    send_output_frame(socket_fd, NULL, 0);
    send_u32(socket_fd, (uint32_t)status);
    return 0;
}

static void handle_client(int socket_fd, const char* executable, const struct DeviceSelection* required)
{
    struct NetworkRequest request;
    char image_path[PATH_MAX] = { 0 };

    if (receive_request(socket_fd, &request) != 0) {
        send_error_result(socket_fd, "Error: invalid or incomplete network request.\n");
        free_request(&request);
        return;
    }
    if (validate_request(&request) != 0) {
        send_error_result(socket_fd, "Error: unsupported or invalid remote command.\n");
        free_request(&request);
        return;
    }
    if (validate_device_selection(&request, required) != 0) {
        char message[NETWORK_MAX_ARG_LENGTH + 160];
        snprintf(message, sizeof(message), "Error: server requires %s %s; client must specify the same selector exactly once.\n",
            required->option, required->value);
        send_error_result(socket_fd, message);
        free_request(&request);
        return;
    }
    if (request.image_index != NETWORK_NO_IMAGE
        && receive_image(socket_fd, request.image_size, image_path, sizeof(image_path)) != 0) {
        send_error_result(socket_fd, "Error: failed to receive the image.\n");
        free_request(&request);
        return;
    }

    if (run_local_command(socket_fd, executable, &request, image_path, required) != 0)
        send_error_result(socket_fd, "Error: failed to start the local flash command.\n");
    if (image_path[0] != '\0')
        unlink(image_path);
    free_request(&request);
}

static int run_server(const char* endpoint, const char* executable, const struct DeviceSelection* required)
{
    int listener = open_listener(endpoint);
    if (listener < 0)
        return 1;

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0 && errno == EINTR)
            continue;
        if (client < 0) {
            fprintf(stderr, "Error: failed to accept a client: %s.\n", strerror(errno));
            close(listener);
            return 1;
        }
        printf("Client connected.\n");
        handle_client(client, executable, required);
        close(client);
        printf("Client disconnected.\n");
    }
}

int NetworkHandleMode(int argc, char* argv[])
{
    const char* listen_endpoint = NULL;
    const char* server_endpoint = NULL;
    int server_index = -1;
    int server_has_separate_value = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --listen needs [ADDRESS:]PORT.\n");
                return 1;
            }
            listen_endpoint = argv[i];
        } else if (strncmp(argv[i], "--listen=", 9) == 0) {
            listen_endpoint = argv[i] + 9;
        } else if (strcmp(argv[i], "--server") == 0) {
            server_index = i;
            server_has_separate_value = 1;
            if (++i >= argc) {
                fprintf(stderr, "Error: --server needs HOST:PORT.\n");
                return 1;
            }
            server_endpoint = argv[i];
        } else if (strncmp(argv[i], "--server=", 9) == 0) {
            server_index = i;
            server_endpoint = argv[i] + 9;
        }
    }

    if (listen_endpoint != NULL && server_endpoint != NULL) {
        fprintf(stderr, "Error: --listen and --server cannot be used together.\n");
        return 1;
    }
    if (listen_endpoint == NULL && server_endpoint == NULL)
        return NETWORK_MODE_NOT_REQUESTED;

    signal(SIGPIPE, SIG_IGN);
    if (listen_endpoint != NULL) {
        struct DeviceSelection required;
        if (parse_device_selection(argc - 1, argv + 1, NETWORK_NO_IMAGE, &required) != 0) {
            fprintf(stderr, "Error: specify at most one valid --device (1-16 in hexadecimal) or --device-SN for the server.\n");
            return 1;
        }
        return run_server(listen_endpoint, argv[0], &required);
    }
    return run_client(server_endpoint, argc, argv, server_index, server_has_separate_value);
}
