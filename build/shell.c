#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

int last_global_status = 0;

typedef enum {
    TOKEN_WORD,
    TOKEN_SEMICOLON,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} Token;

typedef enum {
    OP_NONE,
    OP_SEMICOLON,
    OP_AND,
    OP_OR
} OpType;

typedef struct Command Command;
typedef struct ListElement ListElement;

struct ListElement {
    Command *cmd;
    OpType op;
};

struct Command {
    int negate;
    int is_subshell;
    // Simple command
    int argc;
    char **argv;
    // Subshell
    ListElement *subshell_cmds;
    int subshell_cmd_count;
};

// Prototypes
int count_paren_nesting(const char *str);
int ends_with_operator(const char *str);
char *read_command_line(void);
Token *tokenize(const char *input, int *token_count);
Command *parse_command(Token *tokens, int start, int end, int *consumed);
ListElement *parse_list(Token *tokens, int start, int end, int *element_count);
void free_command(Command *cmd);
void free_list(ListElement *elements, int count);
int execute_list(ListElement *elements, int count);
int execute_command(Command *cmd);
int resolve_logical_path(const char *target, char *resolved, size_t max_len);



int count_paren_nesting(const char *str) {
    int nest = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '(') {
            nest++;
        } else if (str[i] == ')') {
            nest--;
        }
    }
    return nest;
}

int resolve_logical_path(const char *target, char *resolved, size_t max_len) {
    char *old_pwd = getenv("PWD");
    char temp_cwd[1024];
    if (!old_pwd) {
        if (getcwd(temp_cwd, sizeof(temp_cwd))) {
            old_pwd = temp_cwd;
        } else {
            old_pwd = "/";
        }
    }
    
    char raw_path[2048];
    if (target[0] == '/') {
        snprintf(raw_path, sizeof(raw_path), "%s", target);
    } else {
        snprintf(raw_path, sizeof(raw_path), "%s/%s", old_pwd, target);
    }
    
    char *parts[128];
    int part_count = 0;
    
    char *raw_copy = strdup(raw_path);
    char *token = strtok(raw_copy, "/");
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            // do nothing
        } else if (strcmp(token, "..") == 0) {
            if (part_count > 0) {
                part_count--;
            }
        } else {
            if (part_count < 128) {
                parts[part_count++] = token;
            }
        }
        token = strtok(NULL, "/");
    }
    
    char clean_path[2048] = "/";
    int pos = 1;
    for (int i = 0; i < part_count; i++) {
        int len = strlen(parts[i]);
        if (pos + len + 2 > (int)sizeof(clean_path)) break;
        strcpy(clean_path + pos, parts[i]);
        pos += len;
        if (i < part_count - 1) {
            clean_path[pos++] = '/';
        }
    }
    clean_path[pos] = '\0';
    
    strncpy(resolved, clean_path, max_len);
    free(raw_copy);
    return 0;
}



int ends_with_operator(const char *str) {
    int len = strlen(str);
    int i = len - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\t' || str[i] == '\n' || str[i] == '\r')) {
        i--;
    }
    if (i >= 1) {
        if (str[i] == '&' && str[i-1] == '&') return 1;
        if (str[i] == '|' && str[i-1] == '|') return 1;
    }
    return 0;
}

char *read_command_line(void) {
    char *buffer = NULL;
    size_t length = 0;
    int is_first_line = 1;
    int last_had_backslash = 0;
    
    while (1) {
        if (is_first_line) {
            // Bonus: print in red if last command failed
            if (last_global_status == 0) {
                fprintf(stderr, "$ ");
            } else {
                fprintf(stderr, "\033[31m$\033[0m ");
            }
        } else {
            fprintf(stderr, "> ");
        }
        fflush(stderr);
        
        char *line = NULL;
        size_t line_cap = 0;
        ssize_t line_len = getline(&line, &line_cap, stdin);
        
        if (line_len < 0) {
            free(line);
            if (length > 0) {
                return buffer;
            }
            free(buffer);
            return NULL;
        }
        
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[line_len - 1] = '\0';
            line_len--;
        }
        
        int has_backslash = 0;
        if (line_len > 0 && line[line_len - 1] == '\\') {
            has_backslash = 1;
            line[line_len - 1] = '\0';
            line_len--;
        }
        
        if (!buffer) {
            buffer = strdup(line);
            length = line_len;
        } else {
            size_t new_len = length + line_len + 2;
            buffer = realloc(buffer, new_len);
            if (last_had_backslash) {
                strcpy(buffer + length, line);
                length = length + line_len;
            } else {
                buffer[length] = '\n';
                strcpy(buffer + length + 1, line);
                length = length + 1 + line_len;
            }
        }
        
        free(line);
        
        int nest = count_paren_nesting(buffer);
        int has_op = ends_with_operator(buffer);
        
        if (has_backslash || nest > 0 || has_op) {
            is_first_line = 0;
            last_had_backslash = has_backslash;
            continue;
        } else {
            break;
        }
    }
    return buffer;
}

Token *tokenize(const char *input, int *token_count) {
    int cap = 32;
    int count = 0;
    Token *tokens = malloc(cap * sizeof(Token));
    
    const char *p = input;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        
        if (*p == '\n') {
            int ignore_nl = 0;
            if (count > 0) {
                TokenType last_type = tokens[count - 1].type;
                if (last_type == TOKEN_AND || last_type == TOKEN_OR || last_type == TOKEN_SEMICOLON) {
                    ignore_nl = 1;
                }
            } else {
                ignore_nl = 1;
            }
            
            if (ignore_nl) {
                p++;
                continue;
            }
            
            if (count >= cap) {
                cap *= 2;
                tokens = realloc(tokens, cap * sizeof(Token));
            }
            tokens[count].type = TOKEN_SEMICOLON;
            tokens[count].text = strdup(";");
            count++;
            p++;
            continue;
        }
        
        if (count >= cap) {
            cap *= 2;
            tokens = realloc(tokens, cap * sizeof(Token));
        }
        
        if (*p == ';') {
            tokens[count].type = TOKEN_SEMICOLON;
            tokens[count].text = strdup(";");
            count++;
            p++;
        } else if (*p == '&') {
            if (p[1] == '&') {
                tokens[count].type = TOKEN_AND;
                tokens[count].text = strdup("&&");
                count++;
                p += 2;
            } else {
                tokens[count].type = TOKEN_WORD;
                tokens[count].text = strdup("&");
                count++;
                p++;
            }
        } else if (*p == '|') {
            if (p[1] == '|') {
                tokens[count].type = TOKEN_OR;
                tokens[count].text = strdup("||");
                count++;
                p += 2;
            } else {
                tokens[count].type = TOKEN_WORD;
                tokens[count].text = strdup("|");
                count++;
                p++;
            }
        } else if (*p == '(') {
            tokens[count].type = TOKEN_LPAREN;
            tokens[count].text = strdup("(");
            count++;
            p++;
        } else if (*p == ')') {
            tokens[count].type = TOKEN_RPAREN;
            tokens[count].text = strdup(")");
            count++;
            p++;
        } else {
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
                   *p != ';' && *p != '&' && *p != '|' && *p != '(' && *p != ')') {
                p++;
            }
            int len = p - start;
            char *word = malloc(len + 1);
            memcpy(word, start, len);
            word[len] = '\0';
            
            tokens[count].type = TOKEN_WORD;
            tokens[count].text = word;
            count++;
        }
    }
    
    if (count >= cap) {
        cap += 1;
        tokens = realloc(tokens, cap * sizeof(Token));
    }
    tokens[count].type = TOKEN_EOF;
    tokens[count].text = strdup("");
    count++;
    
    *token_count = count;
    return tokens;
}

Command *parse_command(Token *tokens, int start, int end, int *consumed) {
    if (start >= end) {
        *consumed = 0;
        return NULL;
    }
    
    int negate = 0;
    int curr = start;
    
    while (curr < end && tokens[curr].type == TOKEN_WORD && strcmp(tokens[curr].text, "!") == 0) {
        negate = !negate;
        curr++;
    }
    
    if (curr >= end) {
        *consumed = curr - start;
        return NULL;
    }
    
    Command *cmd = calloc(1, sizeof(Command));
    cmd->negate = negate;
    
    if (tokens[curr].type == TOKEN_LPAREN) {
        cmd->is_subshell = 1;
        int lparen_idx = curr;
        int rparen_idx = -1;
        int nest = 1;
        curr++;
        
        while (curr < end) {
            if (tokens[curr].type == TOKEN_LPAREN) {
                nest++;
            } else if (tokens[curr].type == TOKEN_RPAREN) {
                nest--;
                if (nest == 0) {
                    rparen_idx = curr;
                    break;
                }
            }
            curr++;
        }
        
        if (rparen_idx == -1) {
            fprintf(stderr, "syntax error: unmatched '('\n");
            free(cmd);
            *consumed = 0;
            return NULL;
        }
        
        int sub_cmd_count = 0;
        cmd->subshell_cmds = parse_list(tokens, lparen_idx + 1, rparen_idx, &sub_cmd_count);
        cmd->subshell_cmd_count = sub_cmd_count;
        
        *consumed = rparen_idx - start + 1;
        return cmd;
    } else {
        cmd->is_subshell = 0;
        int word_cap = 8;
        cmd->argv = malloc(word_cap * sizeof(char *));
        cmd->argc = 0;
        
        while (curr < end && tokens[curr].type == TOKEN_WORD) {
            if (cmd->argc >= word_cap - 1) {
                word_cap *= 2;
                cmd->argv = realloc(cmd->argv, word_cap * sizeof(char *));
            }
            cmd->argv[cmd->argc++] = strdup(tokens[curr].text);
            curr++;
        }
        cmd->argv[cmd->argc] = NULL;
        
        if (cmd->argc == 0) {
            free(cmd->argv);
            free(cmd);
            *consumed = curr - start;
            return NULL;
        }
        
        *consumed = curr - start;
        return cmd;
    }
}

ListElement *parse_list(Token *tokens, int start, int end, int *element_count) {
    int cap = 8;
    int count = 0;
    ListElement *elements = malloc(cap * sizeof(ListElement));
    
    int curr = start;
    while (curr < end) {
        int consumed = 0;
        Command *cmd = parse_command(tokens, curr, end, &consumed);
        if (!cmd) {
            if (consumed > 0) {
                curr += consumed;
                continue;
            }
            break;
        }
        curr += consumed;
        
        OpType op = OP_NONE;
        if (curr < end) {
            if (tokens[curr].type == TOKEN_SEMICOLON) {
                op = OP_SEMICOLON;
                curr++;
            } else if (tokens[curr].type == TOKEN_AND) {
                op = OP_AND;
                curr++;
            } else if (tokens[curr].type == TOKEN_OR) {
                op = OP_OR;
                curr++;
            }
        }
        
        if (count >= cap) {
            cap *= 2;
            elements = realloc(elements, cap * sizeof(ListElement));
        }
        elements[count].cmd = cmd;
        elements[count].op = op;
        count++;
    }
    
    *element_count = count;
    return elements;
}

void free_command(Command *cmd) {
    if (!cmd) return;
    if (cmd->is_subshell) {
        free_list(cmd->subshell_cmds, cmd->subshell_cmd_count);
    } else {
        for (int i = 0; i < cmd->argc; i++) {
            free(cmd->argv[i]);
        }
        free(cmd->argv);
    }
    free(cmd);
}

void free_list(ListElement *elements, int count) {
    for (int i = 0; i < count; i++) {
        free_command(elements[i].cmd);
    }
    free(elements);
}

int execute_command(Command *cmd) {
    if (!cmd) return 0;
    
    if (cmd->is_subshell) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child
            int sub_status = execute_list(cmd->subshell_cmds, cmd->subshell_cmd_count);
            exit(sub_status);
        } else if (pid < 0) {
            perror("fork");
            return 1;
        } else {
            // Parent
            int status;
            waitpid(pid, &status, 0);
            int exit_code = 1;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            }
            return cmd->negate ? (exit_code == 0 ? 1 : 0) : exit_code;
        }
    } else {
        if (cmd->argc == 0) return 0;
        
        // Builtin: cd
        if (strcmp(cmd->argv[0], "cd") == 0) {
            char *target = NULL;
            if (cmd->argc <= 1 || cmd->argv[1] == NULL || strcmp(cmd->argv[1], "~") == 0 || strcmp(cmd->argv[1], "~/") == 0) {
                target = getenv("HOME");
                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    return cmd->negate ? 0 : 1;
                }
            } else {
                target = cmd->argv[1];
            }
            char resolved[2048];
            resolve_logical_path(target, resolved, sizeof(resolved));
            if (chdir(resolved) != 0) {
                perror("cd");
                return cmd->negate ? 0 : 1;
            }
            setenv("PWD", resolved, 1);
            return cmd->negate ? 1 : 0;
        }
        
        // Builtin: exec
        if (strcmp(cmd->argv[0], "exec") == 0) {
            if (cmd->argc > 1) {
                execvp(cmd->argv[1], &cmd->argv[1]);
                perror("exec");
                return cmd->negate ? 0 : 1;
            }
            return cmd->negate ? 1 : 0;
        }
        
        // Builtin: exit
        if (strcmp(cmd->argv[0], "exit") == 0) {
            int exit_code = last_global_status;
            if (cmd->argc > 1) {
                exit_code = atoi(cmd->argv[1]);
            }
            exit(exit_code);
        }
        
        // External command
        pid_t pid = fork();
        if (pid == 0) {
            execvp(cmd->argv[0], cmd->argv);
            if (errno == ENOENT) {
                fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
                exit(127);
            } else {
                perror(cmd->argv[0]);
                exit(126);
            }
        } else if (pid < 0) {
            perror("fork");
            return cmd->negate ? 0 : 1;
        } else {
            int status;
            waitpid(pid, &status, 0);
            int exit_code = 1;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            }
            return cmd->negate ? (exit_code == 0 ? 1 : 0) : exit_code;
        }
    }
}

int execute_list(ListElement *elements, int count) {
    int last_status = last_global_status;
    int skip = 0;
    
    for (int i = 0; i < count; i++) {
        Command *cmd = elements[i].cmd;
        OpType op = elements[i].op;
        
        if (!skip) {
            last_status = execute_command(cmd);
        }
        
        if (skip) {
            if (op == OP_SEMICOLON) {
                skip = 0;
            } else if (op == OP_AND) {
                if (last_status == 0) {
                    skip = 0;
                }
            } else if (op == OP_OR) {
                if (last_status != 0) {
                    skip = 0;
                }
            }
        } else {
            if (op == OP_AND && last_status != 0) {
                skip = 1;
            } else if (op == OP_OR && last_status == 0) {
                skip = 1;
            }
        }
    }
    return last_status;
}

int main(void) {
    if (!getenv("PWD")) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            setenv("PWD", cwd, 1);
        }
    }
    while (1) {
        char *line = read_command_line();
        if (!line) {
            break;
        }
        
        int token_count = 0;
        Token *tokens = tokenize(line, &token_count);
        
        // If empty input (just EOF token)
        if (token_count <= 1) {
            free(line);
            if (tokens) {
                for (int i = 0; i < token_count; i++) {
                    free(tokens[i].text);
                }
                free(tokens);
            }
            continue;
        }
        
        int count = 0;
        ListElement *elements = parse_list(tokens, 0, token_count - 1, &count);
        
        if (count > 0) {
            last_global_status = execute_list(elements, count);
        }
        
        free_list(elements, count);
        
        for (int i = 0; i < token_count; i++) {
            free(tokens[i].text);
        }
        free(tokens);
        free(line);
    }
    
    // Print newline on EOF just like typical shells
    fprintf(stderr, "\n");
    return last_global_status;
}
