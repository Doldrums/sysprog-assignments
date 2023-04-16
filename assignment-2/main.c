#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  const char *name;
  const char **argv;
  bool redirect;
  const char *redirect_file;
  bool redirect_appending;
  int argc;
} cmd;

int skip_whitespaces(char *str, int start, int end) {
  int cursor = start;
  while (cursor < end && (str[cursor] == ' ' || str[cursor] == '\t')) {
    cursor++;
  }
  return cursor;
}

char *read_token(char *str, int *start, int end) {
  int cursor = *start;

  int token_len = 0;
  int token_start = cursor;

  if (str[cursor] == '"' || str[cursor] == '\'') {
    char quote = str[cursor];
    token_start++;
    cursor++;
    while (str[cursor] != quote) {
      if (str[cursor] == '\\') {
        cursor++;
      }
      cursor++;
      token_len++;

      if (cursor == end) {
        printf("Error: unterminated %c.\n", quote);
        exit(EXIT_FAILURE);
      }
    }
    token_len--;
  } else {
    while (cursor < end && str[cursor] != ' ') {
      if (str[cursor] == '\\') {
        cursor++;
      }
      cursor++;
      token_len++;
    }
  }

  char *token = malloc(sizeof(char) * (token_len + 1));
  int token_cursor = 0;
  int token_end = cursor;
  cursor = token_start;
  while (cursor < token_end) {
    if (str[cursor] == '\\') {
      cursor++;
    }
    token[token_cursor++] = str[cursor++];
  }

  token[token_cursor] = '\0';
  *start = cursor + 1;

  return token;
}

cmd *parse_command(char *str, int start, int end) {
  cmd *command = malloc(sizeof(cmd));

  int offset = skip_whitespaces(str, start, end);
  command->name = read_token(str, &offset, end);

  int argc = 0;
  int args_offset = offset;
  while (offset < end) {
    offset = skip_whitespaces(str, offset, end);
    if (offset == end) {
      break;
    }

    char *tmp = read_token(str, &offset, end);
    free(tmp);
    argc++;
  }

  command->argc = argc;
  command->argv = malloc(sizeof(char *) * (argc + 2));

  offset = args_offset;
  command->argv[0] = command->name;
  argc = 1;
  while (argc <= command->argc) {
    offset = skip_whitespaces(str, offset, end);
    command->argv[argc] = read_token(str, &offset, end);
    argc++;
  }
  command->argv[argc] = NULL;
  command->redirect = false;
  command->redirect_appending = false;

  return command;
}

cmd **parse_line(char *line, int line_len, int *commands_count) {
  int offset = 0;
  int cursor = 0;

  bool in_quote = false;
  char quote = '~';
  bool escaped = false;

  int commands_len = 2;
  int commands_index = 0;
  cmd **commands = malloc(sizeof(cmd *) * commands_len);

  while (line[cursor] != '\0') {
    if (escaped) {
      escaped = false;
    } else if (line[cursor] == '\\') {
      escaped = true;
    } else if (in_quote && line[cursor] == quote) {
      in_quote = false;
    } else if (!in_quote && (line[cursor] == '"' || line[cursor] == '\'')) {
      in_quote = true;
      quote = line[cursor];
    } else if (!in_quote && line[cursor] == '|') {
      commands[commands_index++] = parse_command(line, offset, cursor);

      if (commands_index == commands_len) {
        commands_len *= 2;
        commands = realloc(commands, sizeof(cmd *) * commands_len);
        if (!commands) {
          printf("Error: memory allocation failed.\n");
          exit(EXIT_FAILURE);
        }
      }
      offset = cursor + 1;
    } else if (!in_quote && line[cursor] == '>') {
      commands[commands_index++] = parse_command(line, offset, cursor);

      if (commands_index == commands_len) {
        commands_len *= 2;
        commands = realloc(commands, sizeof(cmd *) * commands_len);
        if (!commands) {
          printf("Error: memory allocation failed.\n");
          exit(EXIT_FAILURE);
        }
      }
      if (line[cursor + 1] == '>') {
        commands[commands_index - 1]->redirect_appending = true;
        cursor++;
      }

      cursor = skip_whitespaces(line, cursor + 1, line_len);
      if (cursor == line_len) {
        printf("Error: Invalid use of >.\n");
        exit(EXIT_FAILURE);
      }

      commands[commands_index - 1]->redirect = true;
      commands[commands_index - 1]->redirect_file =
          read_token(line, &cursor, line_len);
      ;
      offset = cursor;
    } else if (!in_quote && line[cursor] == '&') {
      if (line[cursor + 1] == '&') {
        // todo: and
        cursor++;
      } else {
        // todo: background
      }
    }
    cursor++;
  }

  if (offset < line_len) {
    commands[commands_index++] = parse_command(line, offset, cursor);
    if (commands_index == commands_len) {
      commands_len *= 2;
      commands = realloc(commands, sizeof(cmd *) * commands_len);
      if (!commands) {
        printf("Error: memory allocation failed.\n");
        exit(EXIT_FAILURE);
      }
    }
  }

  commands[commands_index] = NULL;
  *commands_count = commands_index;

  return commands;
}

char *read_line(int *len) {
  int buffer_size = 64; // initial size of buffer
  char *buffer = malloc(sizeof(char) * buffer_size);
  int index = 0;
  int c;

  if (!buffer) {
    printf("Error: memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }

  while ((c = getchar()) != '\n' && c != EOF) {
    buffer[index++] = c;
    if (index == buffer_size) {
      buffer_size *= 2; // double buffer size if it's full
      buffer = realloc(buffer, sizeof(char) * buffer_size);
      if (!buffer) {
        printf("Error: memory allocation failed.\n");
        exit(EXIT_FAILURE);
      }
    }
  }

  buffer[index] = '\0'; // null-terminate the string
  *len = index;

  return buffer;
}

int main() {
  while (true) {
    char cwd[1024];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      perror("getcwd");
      return 1;
    }

    printf("\x1b[34m%s\x1b[0m ", cwd);

    int line_len = 0;
    char *line = read_line(&line_len);

    int commands_count = 0;
    cmd **commands = parse_line(line, line_len, &commands_count);

    int old_fds[2];
    int new_fds[2];

    int command_pids[commands_count];

    if (commands_count > 1) {
      if (pipe(old_fds) == -1) {
        printf("Error: pipe failed.\n");
        exit(EXIT_FAILURE);
      }
    }

    for (int i = 0; i < commands_count; i++) {
      if (i != commands_count - 1) {
        if (pipe(new_fds) == -1) {
          printf("Error: pipe failed.\n");
          exit(EXIT_FAILURE);
        }
      }
      if (strcmp(commands[i]->name, "cd") == 0) {
        if (chdir(commands[i]->argv[1]) == -1) {
          printf("cd: no such file or directory: %s\n", commands[i]->argv[1]);
        }
        continue;
      }

      command_pids[i] = fork();

      if (command_pids[i] < 0) {
        printf("Error: fork failed.\n");
        exit(EXIT_FAILURE);
      } else if (command_pids[i] == 0) { // child process
        const char *const *const_argv = (const char *const *)commands[i]->argv;
        char *const *argv = (char *const *)const_argv;

        if (commands[i]->redirect) {
          int fd;

          if (commands[i]->redirect_appending) {
            fd = open(commands[i]->redirect_file, O_WRONLY | O_CREAT | O_APPEND,
                      S_IRUSR | S_IWUSR); // open file for writing
          } else {
            fd = open(commands[i]->redirect_file, O_WRONLY | O_CREAT,
                      S_IRUSR | S_IWUSR); // open file for writing
          }

          if (fd == -1) {
            printf("Error: redirect failed.\n");
            exit(EXIT_FAILURE);
          }

          dup2(fd, STDOUT_FILENO); // redirect stdout to file
          close(fd);
        }

        if (i > 0) {
          dup2(old_fds[0], STDIN_FILENO);
          close(old_fds[0]);
          close(old_fds[1]);
        }

        if (i != commands_count - 1) {
          close(new_fds[0]);
          dup2(new_fds[1], STDOUT_FILENO);
          close(new_fds[1]);
        }

        execvp(commands[i]->name, argv);

        exit(EXIT_FAILURE);
      } else { // parent process
        if (i > 0) {
          close(old_fds[0]);
          close(old_fds[1]);
        }

        if (i != commands_count - 1) {
          old_fds[0] = new_fds[0];
          old_fds[1] = new_fds[1];
        }
      }
    }

    if (commands_count > 1) {
      close(old_fds[0]);
      close(old_fds[1]);
    }

    for (int i = 0; i < commands_count; i++) {
      int status;
      waitpid(command_pids[i], &status, 0);
    }

    free(line);
  }

  return 0;
}
