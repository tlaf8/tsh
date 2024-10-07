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

void append(Node **head, const char *key, const char *value) {
    Node *node = newNode(key, value);
    if (node == NULL) {
        perror("newNode failed to initialize a new node.");
        return;
    }

    if (*head == NULL) {
        *head = node;
    } else {
        Node *tail = *head;
        while (tail->next != NULL) tail = tail->next;
        tail->next = node;
    }
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

void update_variable(Node **head, const char *var_name, char *value) {
    if (*head == NULL) {
        append(head, var_name, value);
        return;
    }

    for (Node *curr = *head; curr != NULL; curr = curr->next) {
        if (strcmp(curr->key, var_name) == 0) {
            curr->value = value;
        } else {
            append(head, var_name, value);
            return;
        }
    }
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
        }
        free(path);
        return FALSE;
    }
}

void assign_variable(Node **head, const char *var_name, char *params[]) {
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
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("Forking failed");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return;
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        if (execve(sub_command, sub_params, NULL) == -1) {
            perror("Execve failed in variable assignment");
            return;
        }
    } else {
        close(pipe_fd[1]);
        wait(NULL);
        char buffer[4096];
        ssize_t bytes = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
        if (bytes < 0) {
            perror("Sub command read failed");
            close(pipe_fd[0]);
            return;
        }
        buffer[bytes] = '\0';

        char *output = strdup(buffer);
        if (output != NULL) {
            output[strcspn(output, "\n")] = '\0';
            update_variable(head, var_name, output);
        }
    }
}

int read_line(const int infile, char *buffer, const int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        char tmp;
        const ssize_t r = read(infile, &tmp, 1);
        if (r < 0) return -1;
        if (r == 0 || tmp == '\n') break;
        buffer[i++] = tmp;
    }

    buffer[i] = '\0';
    return i;
}


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
        char *params[numtokens];

        int assign = -1, var = -1, pipe = -1, redir = -1;
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

        // * Handle pipes
        // TODO

        // * Handle redirections
        // TODO

        // * Handle variable assignments
        if (assign > 0) {
            assign_variable(&head, command, params);
        } else {
            // * Fork and execute commands
            // Maybe TODO?
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
