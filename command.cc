#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pwd.h>
#include <glob.h>

#include "command.h"
#include "tokenizer.h"

namespace {
volatile sig_atomic_t childChanged = 0;

void childSignalHandler(int) {
    childChanged = 1;
}

void reapChildrenAndLog() {
    int savedErrno = errno;
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        char message[128];

        int n = std::snprintf(
            message,
            sizeof(message),
            "Child process %ld terminated\n",
            static_cast<long>(pid)
        );

        if (n > 0) {
            int fd = open(
                "myshell.log",
                O_WRONLY | O_CREAT | O_APPEND,
                0644
            );

            if (fd >= 0) {
                (void)write(fd, message, static_cast<size_t>(n));
                close(fd);
            }
        }
    }

    childChanged = 0;
    errno = savedErrno;
}

char *duplicateString(const std::string &s) {
    char *copy = static_cast<char *>(std::malloc(s.size() + 1));

    if (!copy) {
        std::perror("malloc");
        std::exit(EXIT_FAILURE);
    }

    std::memcpy(copy, s.c_str(), s.size() + 1);
    return copy;
}

bool hasWildcard(const std::string &s) {
    return s.find('*') != std::string::npos ||
           s.find('?') != std::string::npos ||
           s.find('[') != std::string::npos;
}

std::vector<std::string> expandWildcard(const std::string &arg) {
    if (!hasWildcard(arg)) {
        return {arg};
    }

    glob_t matches{};
    int result = glob(arg.c_str(), 0, nullptr, &matches);

    std::vector<std::string> expanded;

    if (result == 0) {
        for (size_t i = 0; i < matches.gl_pathc; ++i) {
            expanded.emplace_back(matches.gl_pathv[i]);
        }
    } else {
        expanded.push_back(arg);
    }

    globfree(&matches);
    return expanded;
}

void addArgument(SimpleCommand *simple, const std::string &arg) {
    std::vector<std::string> values = expandWildcard(arg);

    for (const auto &value : values) {
        simple->insertArgument(duplicateString(value));
    }
}

bool isRedirection(TokenType type) {
    return type == TOKEN_REDIRECT ||
           type == TOKEN_APPEND ||
           type == TOKEN_INPUT ||
           type == TOKEN_ERROR ||
           type == TOKEN_REDIRECT_AND_ERROR;
}

void reportSyntaxError(const std::string &message) {
    std::cerr << "myshell: syntax error: " << message << '\n';
}

bool applyFdRedirection(
    const Command &cmd,
    int commandIndex,
    int commandCount
) {
    if (commandIndex == 0 && cmd._inputFile) {
        int fd = open(cmd._inputFile, O_RDONLY);

        if (fd < 0) {
            std::perror(cmd._inputFile);
            return false;
        }

        if (dup2(fd, STDIN_FILENO) < 0) {
            std::perror("dup2 input");
            close(fd);
            return false;
        }

        close(fd);
    }

    if (commandIndex == commandCount - 1 && cmd._outFile) {
        int flags = O_WRONLY | O_CREAT |
                    (cmd._append ? O_APPEND : O_TRUNC);

        int fd = open(cmd._outFile, flags, 0644);

        if (fd < 0) {
            std::perror(cmd._outFile);
            return false;
        }

        if (dup2(fd, STDOUT_FILENO) < 0) {
            std::perror("dup2 output");
            close(fd);
            return false;
        }

        close(fd);
    }

    if (commandIndex == commandCount - 1 && cmd._errFile) {
        int fd = open(
            cmd._errFile,
            O_WRONLY | O_CREAT |
            (cmd._out_error ? O_APPEND : O_TRUNC),
            0644
        );

        if (fd < 0) {
            std::perror(cmd._errFile);
            return false;
        }

        if (cmd._out_error) {
            if (dup2(fd, STDOUT_FILENO) < 0) {
                std::perror("dup2 stdout");
                close(fd);
                return false;
            }
        }

        if (dup2(fd, STDERR_FILENO) < 0) {
            std::perror("dup2 stderr");
            close(fd);
            return false;
        }

        close(fd);
    }

    return true;
}

bool isBuiltin(const SimpleCommand *sc) {
    if (!sc || sc->_numberOfArguments == 0) {
        return false;
    }

    return std::strcmp(sc->_arguments[0], "cd") == 0 ||
           std::strcmp(sc->_arguments[0], "exit") == 0;
}

bool runBuiltinInParent(Command &cmd) {
    if (cmd._numberOfSimpleCommands != 1) {
        return false;
    }

    SimpleCommand *sc = cmd._simpleCommands[0];

    if (!isBuiltin(sc)) {
        return false;
    }

    const char *name = sc->_arguments[0];

    if (std::strcmp(name, "exit") == 0) {
        std::cout << "Good bye!!" << std::endl;
        std::exit(0);
    }

    if (std::strcmp(name, "cd") == 0) {
        const char *target = nullptr;

        if (sc->_numberOfArguments >= 2) {
            target = sc->_arguments[1];
        } else {
            target = std::getenv("HOME");

            if (!target) {
                struct passwd *pw = getpwuid(getuid());

                if (pw) {
                    target = pw->pw_dir;
                }
            }
        }

        if (!target) {
            std::cerr << "myshell: cd: HOME is not set\n";
        } else if (chdir(target) < 0) {
            std::perror("cd");
        }

        return true;
    }

    return false;
}
}

void parse(std::vector<Token> &tokens) {
    Command command;
    SimpleCommand *simple = new SimpleCommand();
    bool syntaxError = false;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token &token = tokens[i];

        if (token.type == TOKEN_EOF) {
            break;
        }

        if (token.type == TOKEN_PIPE) {
            if (simple->_numberOfArguments == 0) {
                reportSyntaxError("missing command before '|'");
                syntaxError = true;
                break;
            }

            command.insertSimpleCommand(simple);
            simple = new SimpleCommand();
            continue;
        }

        if (isRedirection(token.type)) {
            if (i + 1 >= tokens.size() ||
                tokens[i + 1].type == TOKEN_EOF ||
                tokens[i + 1].type == TOKEN_PIPE ||
                tokens[i + 1].type == TOKEN_BACKGROUND ||
                isRedirection(tokens[i + 1].type)) {
                reportSyntaxError(
                    "redirection operator requires a filename"
                );
                syntaxError = true;
                break;
            }

            const std::string filename = tokens[++i].value;

            if (token.type == TOKEN_INPUT) {
                std::free(command._inputFile);
                command._inputFile = duplicateString(filename);
            } else if (token.type == TOKEN_REDIRECT) {
                std::free(command._outFile);
                command._outFile = duplicateString(filename);
                command._append = 0;
            } else if (token.type == TOKEN_APPEND) {
                std::free(command._outFile);
                command._outFile = duplicateString(filename);
                command._append = 1;
            } else if (token.type == TOKEN_ERROR) {
                std::free(command._errFile);
                command._errFile = duplicateString(filename);
                command._out_error = 0;
            } else if (token.type == TOKEN_REDIRECT_AND_ERROR) {
                std::free(command._errFile);
                command._errFile = duplicateString(filename);
                command._out_error = 1;
            }

            continue;
        }

        if (token.type == TOKEN_BACKGROUND) {
            command._background = 1;

            if (i + 1 < tokens.size() &&
                tokens[i + 1].type != TOKEN_EOF) {
                reportSyntaxError("'&' must appear at the end of the command");
                syntaxError = true;
            }

            break;
        }

        if (token.value.empty()) {
            continue;
        }

        addArgument(simple, token.value);
    }

    if (!syntaxError) {
        if (simple->_numberOfArguments > 0) {
            command.insertSimpleCommand(simple);
            simple = nullptr;
        } else if (command._numberOfSimpleCommands > 0) {
            reportSyntaxError("missing command after '|'");
            syntaxError = true;
        }

        if (!syntaxError &&
            command._numberOfSimpleCommands > 0) {
            command.execute();
        } else {
            command.clear();
        }
    } else {
        command.clear();
    }

    if (simple) {
        for (int j = 0; j < simple->_numberOfArguments; ++j) {
            std::free(simple->_arguments[j]);
        }

        std::free(simple->_arguments);
        delete simple;
    }
}

SimpleCommand::SimpleCommand() {
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;

    _arguments = static_cast<char **>(
        std::malloc(_numberOfAvailableArguments * sizeof(char *))
    );

    if (!_arguments) {
        std::perror("malloc");
        std::exit(EXIT_FAILURE);
    }

    _arguments[0] = nullptr;
}

void SimpleCommand::insertArgument(char *argument) {
    if (_numberOfAvailableArguments <= _numberOfArguments + 1) {
        _numberOfAvailableArguments *= 2;

        char **newArgs = static_cast<char **>(
            std::realloc(
                _arguments,
                _numberOfAvailableArguments * sizeof(char *)
            )
        );

        if (!newArgs) {
            std::perror("realloc");
            std::exit(EXIT_FAILURE);
        }

        _arguments = newArgs;
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = nullptr;
    ++_numberOfArguments;
}

Command::Command() {
    _numberOfAvailableSimpleCommands = 1;
    _numberOfSimpleCommands = 0;

    _simpleCommands = static_cast<SimpleCommand **>(
        std::malloc(
            _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *)
        )
    );

    if (!_simpleCommands) {
        std::perror("malloc");
        std::exit(EXIT_FAILURE);
    }

    _outFile = nullptr;
    _inputFile = nullptr;
    _errFile = nullptr;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand) {
    if (_numberOfAvailableSimpleCommands ==
        _numberOfSimpleCommands) {
        _numberOfAvailableSimpleCommands *= 2;

        SimpleCommand **newCommands = static_cast<SimpleCommand **>(
            std::realloc(
                _simpleCommands,
                _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *)
            )
        );

        if (!newCommands) {
            std::perror("realloc");
            std::exit(EXIT_FAILURE);
        }

        _simpleCommands = newCommands;
    }

    _simpleCommands[_numberOfSimpleCommands++] = simpleCommand;
}

void Command::clear() {
    for (int i = 0; i < _numberOfSimpleCommands; ++i) {
        SimpleCommand *sc = _simpleCommands[i];

        for (int j = 0; j < sc->_numberOfArguments; ++j) {
            std::free(sc->_arguments[j]);
        }

        std::free(sc->_arguments);
        delete sc;
    }

    _numberOfSimpleCommands = 0;

    std::free(_outFile);
    std::free(_inputFile);
    std::free(_errFile);

    _outFile = nullptr;
    _inputFile = nullptr;
    _errFile = nullptr;

    _background = 0;
    _append = 0;
    _out_error = 0;
}

void Command::execute() {
    if (_numberOfSimpleCommands == 0) {
        clear();
        return;
    }

    if (runBuiltinInParent(*this)) {
        clear();
        return;
    }

    const int count = _numberOfSimpleCommands;
    std::vector<pid_t> pids;
    pids.reserve(static_cast<size_t>(count));

    int previousRead = -1;
    bool failed = false;

    for (int i = 0; i < count; ++i) {
        int pipeFds[2] = {-1, -1};

        if (i < count - 1 && pipe(pipeFds) < 0) {
            std::perror("pipe");
            failed = true;
            break;
        }

        pid_t pid = fork();

        if (pid < 0) {
            std::perror("fork");

            if (pipeFds[0] >= 0) {
                close(pipeFds[0]);
            }

            if (pipeFds[1] >= 0) {
                close(pipeFds[1]);
            }

            failed = true;
            break;
        }

        if (pid == 0) {
            struct sigaction defaultInt{};
            defaultInt.sa_handler = SIG_DFL;
            sigemptyset(&defaultInt.sa_mask);
            sigaction(SIGINT, &defaultInt, nullptr);

            struct sigaction defaultChld{};
            defaultChld.sa_handler = SIG_DFL;
            sigemptyset(&defaultChld.sa_mask);
            sigaction(SIGCHLD, &defaultChld, nullptr);

            if (previousRead != -1 &&
                dup2(previousRead, STDIN_FILENO) < 0) {
                std::perror("dup2 pipe input");
                _exit(1);
            }

            if (pipeFds[1] != -1 &&
                dup2(pipeFds[1], STDOUT_FILENO) < 0) {
                std::perror("dup2 pipe output");
                _exit(1);
            }

            if (previousRead != -1) {
                close(previousRead);
            }

            if (pipeFds[0] != -1) {
                close(pipeFds[0]);
            }

            if (pipeFds[1] != -1) {
                close(pipeFds[1]);
            }

            if (!applyFdRedirection(*this, i, count)) {
                _exit(1);
            }

            SimpleCommand *sc = _simpleCommands[i];

            if (!sc || sc->_numberOfArguments == 0) {
                _exit(0);
            }

            execvp(sc->_arguments[0], sc->_arguments);

            std::fprintf(
                stderr,
                "myshell: %s: %s\n",
                sc->_arguments[0],
                std::strerror(errno)
            );

            _exit(errno == ENOENT ? 127 : 126);
        }

        pids.push_back(pid);

        if (previousRead != -1) {
            close(previousRead);
        }

        if (pipeFds[1] != -1) {
            close(pipeFds[1]);
        }

        previousRead = pipeFds[0];
    }

    if (previousRead != -1) {
        close(previousRead);
    }

    if (!_background) {
        for (pid_t pid : pids) {
            int status;

            while (waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno == ECHILD) {
                    break;
                }

                std::perror("waitpid");
                break;
            }
        }
    } else {
        for (pid_t pid : pids) {
            std::printf(
                "[background pid %ld]\n",
                static_cast<long>(pid)
            );
        }
    }

    if (failed) {
        std::cerr << "myshell: command could not be fully started\n";
    }

    clear();

    if (childChanged) {
        reapChildrenAndLog();
    }
}

void Command::prompt() {
    struct sigaction ignoreInt{};
    ignoreInt.sa_handler = SIG_IGN;
    sigemptyset(&ignoreInt.sa_mask);
    sigaction(SIGINT, &ignoreInt, nullptr);

    struct sigaction childAction{};
    childAction.sa_handler = childSignalHandler;
    sigemptyset(&childAction.sa_mask);
    childAction.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &childAction, nullptr);

    std::string input;

    while (true) {
        if (childChanged) {
            reapChildrenAndLog();
        }

        std::cout << "myshell> " << std::flush;

        if (!std::getline(std::cin, input)) {
            std::cout << "\n";
            break;
        }

        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand = nullptr;

int main() {
    Command shell;
    shell.prompt();
    return 0;
}
