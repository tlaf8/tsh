#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "parser.h"

typedef struct Node {
    char *key;
    char *value;
    struct Node *next;
} Node;

Node *newNode(const char *key, const char *value);

int append(Node **head, const char *key, const char *value);

void destroy(Node **head);

char *variable_lookup(Node **head, const char *key);

int normalize_executable(char **command);

int update_variable(Node **head, const char *var_name, char *value);

int assign_variable(Node **head, const char *var_name, char *params[]);

int handle_redirect(char *params[], int num_tokens);

int handle_pipe(char *params[], char **output);

int handle_pipe2redirect(char *params[], int num_tokens);

int handle_pipe2var(Node **head, const char* var_name, char *params[], int num_tokens);

int read_line(int infile, char *buffer, int maxlen);


int main(const int argc, char *argv[]) {
    Node *head = NULL;

    if (argc != 2) {
        printf("Usage: %s <input file>\n", argv[0]);
        return -1;
    }

    const int infile = open(argv[1], O_RDONLY);
    if (infile < 0) {
        perror("Error opening input file");
        return -2;
    }

    while (1) {
        char buffer[4096];

        const int readlen = read_line(infile, buffer, 4096);
        if (readlen < 0) {
            perror("Error reading input file");
            return -3;
        }

        if (readlen == 0) break;

        // Tokenize the line
        int numtokens = 0;
        token_t **tokens = tokenize(buffer, readlen, &numtokens);
        assert(numtokens > 0);

        // Parse token list
        // * Organize tokens into command parameters
        char *command = tokens[0]->value;
        char *params[numtokens + 1];

        int assign = -1, pipe = -1, redir = -1;
        for (int i = 0; i < numtokens; i++) {
            if (tokens[i]->type == TOKEN_ASSIGN) { assign = 1; }
            if (tokens[i]->type == TOKEN_PIPE) { pipe = 1; }
            if (tokens[i]->type == TOKEN_REDIR) { redir = 1; }
            if (tokens[i]->type == TOKEN_VAR) {
                char *expanded = variable_lookup(&head, tokens[i]->value);
                if (expanded == NULL) {
                    perror("Unknown variable");
                    return -4;
                } else {
                    params[i] = expanded;
                    continue;
                }
            }
            params[i] = tokens[i]->value;
        }
        params[numtokens] = NULL;

        if (assign > 0 && pipe > 0) {
            handle_pipe2var(&head, command, params, numtokens);
        } else if (assign > 0) {
            assign_variable(&head, command, params);
        } else if (pipe > 0 && redir > 0) {
            handle_pipe2redirect(params, numtokens);
        } else if (pipe > 0) {
            handle_pipe(params, NULL);
        } else if (redir > 0) {
            handle_redirect(params, numtokens);
        } else {
            normalize_executable(&command);
            if (fork() != 0) {
                wait(0);
            } else {
                if (execve(command, params, NULL) == -1) {
                    perror("Error executing command");
                    exit(EXIT_FAILURE);
                }
            }
        }

        // Free tokens vector
        for (int ii = 0; ii < numtokens; ii++) {
            free(tokens[ii]->value);
            free(tokens[ii]);
        }
        free(tokens);
    }

    close(infile);
    destroy(&head);

    // Remember to deallocate anything left which was allocated dynamically
    // (i.e., using malloc, realloc, strdup, etc.)

    return 0;
}


Node *newNode(const char *key, const char *value) {
    Node *node = (Node *) malloc(sizeof(Node));
    if (node == NULL) {
        perror("Malloc failed creating new node.");
        return NULL;
    }

    node->key = strdup(key);
    node->value = strdup(value);

    if (node->key == NULL || node->value == NULL) {
        free(node->key);
        free(node->value);
        free(node);
        perror("Node's key and/or value failed to duplicate.");
        return NULL;
    }

    node->next = NULL;
    return node;
}

int append(Node **head, const char *key, const char *value) {
    Node *node = newNode(key, value);
    if (node == NULL) {
        perror("newNode failed to initialize a new node.");
        exit(-1);
    }

    if (*head == NULL) {
        *head = node;
    } else {
        Node *tail = *head;
        while (tail->next != NULL) tail = tail->next;
        tail->next = node;
    }

    return 0;
}

void destroy(Node **head) {
    while (*head != NULL) {
        Node *doomed = *head;
        *head = (*head)->next;
        free(doomed->key);
        free(doomed->value);
        free(doomed);
    }
    *head = NULL;
}

char *variable_lookup(Node **head, const char *key) {
    for (const Node *curr = *head; curr != NULL; curr = curr->next) {
        if (strcmp(curr->key, key) == 0) {
            return curr->value;
        }
    }
    return NULL;
}

int normalize_executable(char **command) {
    if ((*command)[0] == '/') return TRUE;

    if (strchr(*command, '/') != NULL) {
        char cwd[1028];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("Cannot obtain current working directory");
            return FALSE;
        }

        char *new_cmd = malloc(strlen(cwd) + strlen(*command) + 2);
        if (new_cmd == NULL) {
            perror("Malloc failed to initialize new command");
            return FALSE;
        }

        snprintf(new_cmd, strlen(cwd) + strlen(*command) + 2, "%s/%s", cwd, *command);
        *command = new_cmd;
        return TRUE;
    } else {
        char *path_env = getenv("PATH");
        if (path_env == NULL) {
            perror("Unable to obtain PATH");
            return FALSE;
        }

        char *path = strdup(path_env);
        if (path == NULL) {
            perror("Failed to duplicate PATH");
            return FALSE;
        }

        for (char *dir = strtok(path, ":"); dir != NULL; dir = strtok(NULL, ":")) {
            size_t path_size = strlen(dir) + strlen(*command) + 2;
            char *test_path = malloc(path_size);
            snprintf(test_path, path_size, "%s/%s", dir, *command);

            if (access(test_path, X_OK) == 0) {
                *command = test_path;
                free(path);
                return TRUE;
            }
            free(test_path);
        }
        free(path);
        return FALSE;
    }
}

int update_variable(Node **head, const char *var_name, char *value) {
    if (*head == NULL) {
        return append(head, var_name, value);
    } else {
        Node *curr = *head;
        while (curr != NULL) {
            if (strcmp(curr->key, var_name) == 0) {
                free(curr->value);
                curr->value = strdup(value);
                if (curr->value == NULL) {
                    perror("Failed to duplicate value in update_variable");
                    return -1;
                }
                return 0;
            }
            curr = curr->next;
        }
        return append(head, var_name, value);
    }
}

int assign_variable(Node **head, const char *var_name, char *params[]) {
    char *sub_command = params[2];

    int num_params = 0;
    for (int i = 2; params[i++] != NULL; num_params++) {}

    char *sub_params[num_params + 1];
    for(int i = 2; params[i] != NULL; i++) {
        sub_params[i - 2] = params[i];
    }
    sub_params[num_params] = NULL;
    normalize_executable(&sub_command);

    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("Pipe creation failed");
        exit(-1);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("Forking failed");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        exit(-1);
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        if (execve(sub_command, sub_params, NULL) == -1) {
            perror("Execve failed in variable assignment");
            exit(-1);
        }
    } else {
        close(pipe_fd[1]);
        wait(NULL);
        char buffer[4096];
        ssize_t bytes = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
        if (bytes < 0) {
            perror("Sub command read failed");
            close(pipe_fd[0]);
            exit(-1);
        }
        buffer[bytes] = '\0';

        char *output = strdup(buffer);
        if (output != NULL) {
            output[strcspn(output, "\n")] = '\0';
            update_variable(head, var_name, output);
            free(output);
        }
    }
    return 0;
}

int handle_redirect(char *params[], const int num_tokens) {
    char *output_file = params[num_tokens - 1];
    char *sub_command = params[0];

    int num_params = 0;
    for (int i = 0; params[i++] != NULL; num_params++) {}

    char *sub_params[num_params + 1];
    for (int i = 0; params[i] != NULL; i++) {
        sub_params[i] = params[i];
    }
    sub_params[num_params] = NULL;
    normalize_executable(&sub_command);

    int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open or create file for writing");
        exit(-1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("Failed to fork while processing redirect");
        close(fd);
        exit(-1);
    }

    if (pid == 0) {
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("Failed to redirect stdout");
            close(fd);
            exit(-1);
        }
        close(fd);

        if (execve(sub_command, sub_params, NULL) == -1) {
            perror("Failed to execute command while handling redirect");
            exit(-1);
        }
    } else {
        close(fd);
        wait(NULL);
    }

    return 0;
}

int handle_pipe(char *params[], char **output) {
    int src_num_params = 0;
    for (int i = 0; params[i++] != NULL; src_num_params++) {}

    char *sub_params_src[src_num_params + 1];
    for (int i = 0; params[i] != NULL; i++) {
        sub_params_src[i] = params[i];
    }
    sub_params_src[src_num_params] = NULL;
    char *sub_command_src = sub_params_src[0];
    normalize_executable(&sub_command_src);

    int dest_num_params = 0;
    for (int i = src_num_params + 1; params[i++] != NULL; dest_num_params++) {}

    char *sub_params_dest[dest_num_params + 1];
    for(int i = 0; params[src_num_params + 1 + i] != NULL; i++) {
        sub_params_dest[i] = params[src_num_params + 1 + i];
    }
    sub_params_dest[dest_num_params] = NULL;
    char *sub_command_dest = sub_params_dest[0];
    normalize_executable(&sub_command_dest);

    int output_pipe[2];
    int pipefd[2];
    if (pipe(pipefd) == -1 || pipe(output_pipe) == -1) {
        perror("Failed to create pipe");
        exit(-1);
    }

    pid_t pid1 = fork();
    if (pid1 == -1) {
        perror("Failed forking first child process");
        exit(-1);
    }

    if (pid1 == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);

        execve(sub_command_src, sub_params_src, NULL);
        perror("execve failed running source");
        exit(-1);
    }

    pid_t pid2 = fork();
    if (pid2 == -1) {
        perror("fork");
        exit(-1);
    }

    if (pid2 == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[1]);

        execve(sub_command_dest, sub_params_dest, NULL);
        perror("execve failed running destination");
        exit(-1);
    }

    close(pipefd[0]);
    close(pipefd[1]);
    close(output_pipe[1]);

    ssize_t bytes_read = 0;
    size_t total_bytes = 0;
    size_t buffer_size = 4096;
    char *buffer = malloc(buffer_size);
    if (!buffer) {
        perror("Failed to allocate buffer");
        exit(-1);
    }

    while ((bytes_read = read(output_pipe[0], buffer + total_bytes, buffer_size - total_bytes - 1)) > 0) {
        total_bytes += bytes_read;
        if (total_bytes >= buffer_size - 1) {
            buffer_size *= 2;
            char *new_buffer = realloc(buffer, buffer_size);
            if (!new_buffer) {
                perror("Failed to reallocate buffer");
                free(buffer);
                exit(-1);
            }
            buffer = new_buffer;
        }
    }

    if (bytes_read == -1) {
        perror("Read from second command failed");
        free(buffer);
        exit(-1);
    }

    buffer[total_bytes] = '\0';

    if (output == NULL) {
        write(STDOUT_FILENO, buffer, strlen(buffer));
        free(buffer);
    } else {
        buffer[strlen(buffer) > 0 && buffer[strlen(buffer) - 1] == '\n' ? strlen(buffer) - 1 : strlen(buffer)] = '\0';
        *output = buffer;
    }

    close(output_pipe[0]);

    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);

    return 0;
}

int handle_pipe2redirect(char *params[], const int num_tokens) {
    char *output_file = params[num_tokens - 1];
    int src_num_params = 0;
    for (int i = 0; params[i++] != NULL; src_num_params++) {}

    char *sub_params_src[src_num_params + 1];
    for (int i = 0; params[i] != NULL; i++) {
        sub_params_src[i] = params[i];
    }
    sub_params_src[src_num_params] = NULL;
    char *sub_command_src = sub_params_src[0];
    normalize_executable(&sub_command_src);

    int dest_num_params = 0;
    for (int i = src_num_params + 1; params[i++] != NULL; dest_num_params++) {}

    char *sub_params_dest[dest_num_params + 1];
    for(int i = 0; params[src_num_params + 1 + i] != NULL; i++) {
        sub_params_dest[i] = params[src_num_params + 1 + i];
    }
    sub_params_dest[dest_num_params] = NULL;
    char *sub_command_dest = sub_params_dest[0];
    normalize_executable(&sub_command_dest);

    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        perror("Failed to create pipe");
        exit(-1);
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("Failed forking on first command");
        exit(-1);
    }

    if (pid1 == 0) {
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[0]);
        close(pipe_fd[1]);

        if (execve(sub_command_src, sub_params_src, NULL) == -1) {
            perror("Failed to execute first command while handling pipe");
            exit(-1);
        }
    }

    pid_t pid2 = fork();
    if (pid2 < 0) {
        perror("Failed forking on second command");
        exit(-1);
    }

    if (pid2 == 0) {
        int output_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output_fd < 0) {
            perror("Failed to open output file");
            exit(EXIT_FAILURE);
        }

        dup2(pipe_fd[0], STDIN_FILENO);
        dup2(output_fd, STDOUT_FILENO);
        close(pipe_fd[1]);
        close(pipe_fd[0]);
        close(output_fd);

        if (execve(sub_command_dest, sub_params_dest, NULL) == -1) {
            perror("Failed to execute second command while handling pipe");
            exit(EXIT_FAILURE);
        }
    }

    close(pipe_fd[0]);
    close(pipe_fd[1]);

    wait(NULL);
    wait(NULL);

    return 0;
}

int handle_pipe2var(Node **head, const char* var_name, char *params[], const int num_tokens) {
    char *sub_params[num_tokens - 1];
    for(int i = 2; i < num_tokens; i++) {
        sub_params[i - 2] = params[i];
    }
    sub_params[num_tokens - 2] = NULL;

    char *command_output;
    handle_pipe(sub_params, &command_output);

    update_variable(head, var_name, command_output);

    free(command_output);
    return 0;
}

int read_line(const int infile, char *buffer, const int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        char tmp;
        const ssize_t r = read(infile, &tmp, 1);
        if (r < 0) {
            perror("Failed reading file");
            exit(-1);
        }
        if (r == 0 || tmp == '\n') break;
        buffer[i++] = tmp;
    }

    buffer[i] = '\0';
    return i;
}
