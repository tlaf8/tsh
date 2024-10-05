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

Node *newNode(char *key, char *value) {
    Node *node = malloc(sizeof(Node));
    node->key = key;
    node->value = value;
    node->next = NULL;
    return node;
}

void append(Node **head, char *key, char *value) {
    Node *node = newNode(key, value);
    if (*head == NULL) {
        *head = node;
    } else {
        Node *tail = *head;
        while (tail->next != NULL) tail = tail->next;
        tail->next = node;
    }
}

void destroy(Node *head) {
    while (head != NULL) {
        Node *doomed = head;
        head = head->next;
        free(doomed);
    }
}

int read_line(const int infile, char *buffer, const int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        char tmp;
        const int r = read(infile, &tmp, 1);
        if (r < 0) return -1;
        if (r == 0 || tmp == '\n') break;
        buffer[i++] = tmp;
    }

    buffer[i] = '\0';
    return i;
}

int normalize_executable(char **command) {
    if ((*command)[0] == '/') return TRUE;

    const char *bin_path = "/bin/";
    char *new_cmd = malloc(strlen(bin_path) + strlen(*command + 1));
    if (new_cmd == NULL) {
        perror("Malloc failed while normalizing command.");
        return FALSE;
    }

    snprintf(new_cmd, strlen(bin_path) + strlen(*command) + 1, "%s%s", bin_path, *command);
    *command = new_cmd;
    return TRUE;
}

void update_variable(Node *head, char *key, char *value) {
    for (Node *curr = head; curr != NULL; curr = curr->next) {
        if (strcmp(curr->key, key) == 0) {
            curr->value = value;
        } else {
            append(&head, key, value);
        }
    }
}

char *lookup_variable(const Node *head, const char *key) {
    for (const Node *curr = head; curr != NULL; curr = curr->next) {
        if (strcmp(curr->key, key) == 0) {
            return curr->value;
        }
    }
    return NULL;
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
        char buffer[1024];

        const int readlen = read_line(infile, buffer, 1024);
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
        int assign = -1, var = -1, pipe = -1, redir = -1;
        for (int i = 0; i < numtokens; i++) {
            if (tokens[i]->type == TOKEN_ASSIGN) { assign = 1; }
            if (tokens[i]->type == TOKEN_VAR) { var = 1; }
            if (tokens[i]->type == TOKEN_PIPE) { pipe = 1; }
            if (tokens[i]->type == TOKEN_REDIR) { redir = 1; }
            params[i] = tokens[i]->value;
        }
        params[numtokens] = NULL;

        // * Handle pipes
        // TODO

        // * Handle redirections
        // TODO

        normalize_executable(&command);

        // * Fork and execute commands
        // Maybe TODO?
        if (fork() != 0) {
            wait(0);
        } else {
            if (execve(command, params, 0) == -1) {
                perror("Error executing command");
                exit(EXIT_FAILURE);
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
    destroy(head);

    // Remember to deallocate anything left which was allocated dynamically
    // (i.e., using malloc, realloc, strdup, etc.)

    return 0;
}
