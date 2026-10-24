#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>
#include <vector>
#include <string>
#include <iostream>

#include "command.h"
#include "tokenizer.h"

static volatile sig_atomic_t childFinished = 0;

static void childSignalHandler(int)
{
    childFinished = 1;
}

static void logChild(pid_t pid)
{
    char line[128];

    int length = snprintf(line, sizeof(line),
                          "Child process %ld terminated\n", (long)pid);

    if (length <= 0)
        return;

    int fd = open("myshell.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd >= 0)
    {
        write(fd, line, (size_t)length);
        close(fd);
    }
}

static void collectFinishedChildren()
{
    int savedErrno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        logChild(pid);

    childFinished = 0;
    errno = savedErrno;
}

static char *copyString(const std::string &value)
{
    char *result = (char *)malloc(value.size() + 1);

    if (result == NULL)
    {
        perror("malloc");
        exit(1);
    }

    memcpy(result, value.c_str(), value.size() + 1);

    return result;
}

static void addArgumentWithWildcard(SimpleCommand *command,
                                    const std::string &value)
{
    if (value.find('*') == std::string::npos &&
        value.find('?') == std::string::npos)
    {
        command->insertArgument(copyString(value));
        return;
    }

    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    int result = glob(value.c_str(), 0, NULL, &matches);

    if (result == 0)
    {
        for (size_t i = 0; i < matches.gl_pathc; ++i)
            command->insertArgument(copyString(matches.gl_pathv[i]));
    }
    else
    {
        command->insertArgument(copyString(value));
    }

    globfree(&matches);
}

static bool isRedirection(TokenType type)
{
    return type == TOKEN_REDIRECT ||
           type == TOKEN_APPEND ||
           type == TOKEN_INPUT ||
           type == TOKEN_ERROR ||
           type == TOKEN_REDIRECT_AND_ERROR;
}

static void syntaxError(const char *message)
{
    fprintf(stderr, "myshell: syntax error: %s\n", message);
}

static bool openAndRedirect(const char *filename, int target,
                            int flags, mode_t mode)
{
    int fd = open(filename, flags, mode);

    if (fd < 0)
    {
        perror(filename);
        return false;
    }

    if (dup2(fd, target) < 0)
    {
        perror("dup2");
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static bool applyInputRedirection(const Command &command)
{
    if (command._inputFile == NULL)
        return true;

    return openAndRedirect(command._inputFile, STDIN_FILENO,
                           O_RDONLY, 0644);
}

static bool applyOutputRedirection(const Command &command)
{
    if (command._outFile == NULL)
        return true;

    int flags = O_WRONLY | O_CREAT;

    if (command._append)
        flags |= O_APPEND;
    else
        flags |= O_TRUNC;

    return openAndRedirect(command._outFile, STDOUT_FILENO,
                           flags, 0644);
}

static bool applyErrorRedirection(const Command &command)
{
    if (command._errFile == NULL)
        return true;

    int flags = O_WRONLY | O_CREAT;

    if (command._out_error)
        flags |= O_APPEND;
    else
        flags |= O_TRUNC;

    int fd = open(command._errFile, flags, 0644);

    if (fd < 0)
    {
        perror(command._errFile);
        return false;
    }

    if (command._out_error && dup2(fd, STDOUT_FILENO) < 0)
    {
        perror("dup2 stdout");
        close(fd);
        return false;
    }

    if (dup2(fd, STDERR_FILENO) < 0)
    {
        perror("dup2 stderr");
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static bool runBuiltin(Command &command)
{
    if (command._numberOfSimpleCommands != 1)
        return false;

    SimpleCommand *simple = command._simpleCommands[0];

    if (simple == NULL || simple->_numberOfArguments == 0)
        return false;

    const char *name = simple->_arguments[0];

    if (strcmp(name, "cd") != 0 && strcmp(name, "exit") != 0)
        return false;

    if (strcmp(name, "exit") == 0)
    {
        printf("Good bye!!\n");
        fflush(stdout);
        exit(0);
    }

    if (strcmp(name, "cd") == 0)
    {
        const char *directory;

        if (simple->_numberOfArguments > 1)
            directory = simple->_arguments[1];
        else
            directory = getenv("HOME");

        if (directory == NULL)
        {
            fprintf(stderr, "myshell: cd: HOME is not set\n");
        }
        else if (chdir(directory) < 0)
        {
            perror("cd");
        }

        return true;
    }

    return false;
}

void parse(std::vector<Token> &tokens)
{
    Command command;
    SimpleCommand *simple = new SimpleCommand();
    bool badSyntax = false;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        Token &token = tokens[i];

        if (token.type == TOKEN_EOF)
            break;

        if (token.type == TOKEN_PIPE)
        {
            if (simple->_numberOfArguments == 0)
            {
                syntaxError("missing command before pipe");
                badSyntax = true;
                break;
            }

            command.insertSimpleCommand(simple);
            simple = new SimpleCommand();
            continue;
        }

        if (isRedirection(token.type))
        {
            if (i + 1 >= tokens.size() ||
                tokens[i + 1].type == TOKEN_EOF ||
                tokens[i + 1].type == TOKEN_PIPE ||
                tokens[i + 1].type == TOKEN_BACKGROUND ||
                isRedirection(tokens[i + 1].type))
            {
                syntaxError("redirection requires a filename");
                badSyntax = true;
                break;
            }

            const char *filename = tokens[++i].value.c_str();

            if (token.type == TOKEN_INPUT)
            {
                free(command._inputFile);
                command._inputFile = copyString(filename);
            }
            else if (token.type == TOKEN_REDIRECT)
            {
                free(command._outFile);
                command._outFile = copyString(filename);
                command._append = 0;
            }
            else if (token.type == TOKEN_APPEND)
            {
                free(command._outFile);
                command._outFile = copyString(filename);
                command._append = 1;
            }
            else if (token.type == TOKEN_ERROR)
            {
                free(command._errFile);
                command._errFile = copyString(filename);
                command._out_error = 0;
            }
            else if (token.type == TOKEN_REDIRECT_AND_ERROR)
            {
                free(command._errFile);
                command._errFile = copyString(filename);
                command._out_error = 1;
            }

            continue;
        }

        if (token.type == TOKEN_BACKGROUND)
        {
            command._background = 1;

            if (i + 1 < tokens.size() &&
                tokens[i + 1].type != TOKEN_EOF)
            {
                syntaxError("background operator must be at the end");
                badSyntax = true;
            }

            break;
        }

        if (!token.value.empty())
            addArgumentWithWildcard(simple, token.value);
    }

    if (!badSyntax)
    {
        if (simple->_numberOfArguments > 0)
        {
            command.insertSimpleCommand(simple);
            simple = NULL;
        }
        else if (command._numberOfSimpleCommands > 0)
        {
            syntaxError("missing command after pipe");
            badSyntax = true;
        }

        if (!badSyntax && command._numberOfSimpleCommands > 0)
            command.execute();
        else
            command.clear();
    }
    else
    {
        command.clear();
    }

    if (simple != NULL)
    {
        for (int i = 0; i < simple->_numberOfArguments; ++i)
            free(simple->_arguments[i]);

        free(simple->_arguments);
        delete simple;
    }
}

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;

    _arguments = (char **)malloc(
        _numberOfAvailableArguments * sizeof(char *));

    if (_arguments == NULL)
    {
        perror("malloc");
        exit(1);
    }

    _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfAvailableArguments <= _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;

        char **newArguments = (char **)realloc(
            _arguments,
            _numberOfAvailableArguments * sizeof(char *));

        if (newArguments == NULL)
        {
            perror("realloc");
            exit(1);
        }

        _arguments = newArguments;
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    ++_numberOfArguments;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    _numberOfSimpleCommands = 0;

    _simpleCommands = (SimpleCommand **)malloc(
        _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    if (_simpleCommands == NULL)
    {
        perror("malloc");
        exit(1);
    }

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;

    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;

        SimpleCommand **newCommands = (SimpleCommand **)realloc(
            _simpleCommands,
            _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

        if (newCommands == NULL)
        {
            perror("realloc");
            exit(1);
        }

        _simpleCommands = newCommands;
    }

    _simpleCommands[_numberOfSimpleCommands++] = simpleCommand;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        SimpleCommand *simple = _simpleCommands[i];

        for (int j = 0; j < simple->_numberOfArguments; ++j)
            free(simple->_arguments[j]);

        free(simple->_arguments);
        delete simple;
    }

    free(_outFile);
    free(_inputFile);
    free(_errFile);

    _numberOfSimpleCommands = 0;

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;

    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n              COMMAND TABLE\n\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        printf("  %-3d ", i);

        for (int j = 0;
             j < _simpleCommands[i]->_numberOfArguments; ++j)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }

        printf("\n");
    }

    printf("\n  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");

    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default",
           _errFile ? _errFile : "default",
           (_out_error && _errFile) ? _errFile : "default",
           _background ? "YES" : "NO");
}

void Command::execute()
{
    if (_numberOfSimpleCommands == 0)
    {
        clear();
        return;
    }

    if (runBuiltin(*this))
    {
        clear();
        return;
    }

    int count = _numberOfSimpleCommands;
    std::vector<pid_t> children;

    int previousRead = -1;
    bool failed = false;

    for (int i = 0; i < count; ++i)
    {
        int pipefd[2] = {-1, -1};

        if (i < count - 1 && pipe(pipefd) < 0)
        {
            perror("pipe");
            failed = true;
            break;
        }

        pid_t pid = fork();

        if (pid < 0)
        {
            perror("fork");

            if (pipefd[0] >= 0)
                close(pipefd[0]);

            if (pipefd[1] >= 0)
                close(pipefd[1]);

            failed = true;
            break;
        }

        if (pid == 0)
        {
            struct sigaction action;
            memset(&action, 0, sizeof(action));

            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);

            sigaction(SIGINT, &action, NULL);
            sigaction(SIGCHLD, &action, NULL);

            if (previousRead >= 0 &&
                dup2(previousRead, STDIN_FILENO) < 0)
            {
                perror("dup2");
                _exit(1);
            }

            if (pipefd[1] >= 0 &&
                dup2(pipefd[1], STDOUT_FILENO) < 0)
            {
                perror("dup2");
                _exit(1);
            }

            if (previousRead >= 0)
                close(previousRead);

            if (pipefd[0] >= 0)
                close(pipefd[0]);

            if (pipefd[1] >= 0)
                close(pipefd[1]);

            if (i == 0 && !applyInputRedirection(*this))
                _exit(1);

            if (i == count - 1 && !applyOutputRedirection(*this))
                _exit(1);

            if (!applyErrorRedirection(*this))
                _exit(1);

            SimpleCommand *simple = _simpleCommands[i];

            execvp(simple->_arguments[0], simple->_arguments);

            int execError = errno;

            fprintf(stderr, "myshell: %s: %s\n",
                    simple->_arguments[0], strerror(execError));

            _exit(execError == ENOENT ? 127 : 126);
        }

        children.push_back(pid);

        if (previousRead >= 0)
            close(previousRead);

        if (pipefd[1] >= 0)
            close(pipefd[1]);

        previousRead = pipefd[0];
    }

    if (previousRead >= 0)
        close(previousRead);

    if (_background)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            printf("[background pid %ld]\n", (long)children[i]);
        }

        fflush(stdout);
    }
    else
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            int status;
            pid_t result;

            do
            {
                result = waitpid(children[i], &status, 0);
            }
            while (result < 0 && errno == EINTR);

            if (result == children[i])
                logChild(children[i]);
            else if (result < 0 && errno != ECHILD)
                perror("waitpid");
        }
    }

    if (failed)
        fprintf(stderr, "myshell: command could not be fully started\n");

    clear();

    if (childFinished)
        collectFinishedChildren();
}

void Command::prompt()
{
    struct sigaction ignoreInterrupt;
    memset(&ignoreInterrupt, 0, sizeof(ignoreInterrupt));

    ignoreInterrupt.sa_handler = SIG_IGN;
    sigemptyset(&ignoreInterrupt.sa_mask);

    sigaction(SIGINT, &ignoreInterrupt, NULL);

    struct sigaction childAction;
    memset(&childAction, 0, sizeof(childAction));

    childAction.sa_handler = childSignalHandler;
    sigemptyset(&childAction.sa_mask);
    childAction.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    sigaction(SIGCHLD, &childAction, NULL);

    std::string input;

    while (true)
    {
        if (childFinished)
            collectFinishedChildren();

        printf("myshell> ");
        fflush(stdout);

        if (!std::getline(std::cin, input))
        {
            printf("\n");
            break;
        }

        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand = NULL;

int main()
{
    Command::_currentCommand.prompt();
    return 0;
}
