#define _POSIX_C_SOURCE 200809L

#include "command_runner.h"
#include "cli_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int run_cmd(const char *const argv[])
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return -1;
    }

    if (pid == 0)
    {
        execvp(argv[0], (char *const *)argv);
        perror("execvp");
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
    {
        perror("waitpid");
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }

    return -1;
}

int run_cmd_silent(const char *const argv[])
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
    {
        return -1;
    }

    if (pid == 0)
    {
        (void)setenv("LC_ALL", "C", 1);
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull)
        {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
    {
        return -1;
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }

    return -1;
}

int capture_cmd(const char *const argv[], char *out, size_t out_size)
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    size_t used = 0;

    if (!out || out_size == 0)
    {
        return -1;
    }

    out[0] = '\0';

    if (pipe(pipefd) != 0)
    {
        return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        (void)setenv("LC_ALL", "C", 1);

        {
            FILE *devnull = fopen("/dev/null", "w");
            if (devnull)
            {
                dup2(fileno(devnull), STDERR_FILENO);
            }
        }

        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);

    while (used + 1 < out_size)
    {
        ssize_t n = read(pipefd[0], out + used, out_size - used - 1);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (n == 0)
        {
            break;
        }

        used += (size_t)n;
    }

    out[used] = '\0';
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) < 0)
    {
        return -1;
    }

    trim_space(out);

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }

    return -1;
}
