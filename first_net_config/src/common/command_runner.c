#define _POSIX_C_SOURCE 200809L

#include "common/command_runner.h"
#include "common/cli_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int child_exit_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_cmd_in_terminal_ui(const char *const argv[])
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    char *output = NULL;
    size_t used = 0;
    size_t capacity = 0;
    char chunk[4096];

    if (pipe(pipefd) != 0)
    {
        printf("[失败] 无法启动系统命令：%s\n", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        printf("[失败] 无法启动系统命令：%s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
    {
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);
    for (;;)
    {
        ssize_t count = read(pipefd[0], chunk, sizeof(chunk));

        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (count == 0)
        {
            break;
        }
        if (used + (size_t)count + 1 > capacity)
        {
            size_t new_capacity = capacity == 0 ? 8192 : capacity * 2;
            char *new_output;

            while (new_capacity < used + (size_t)count + 1)
            {
                new_capacity *= 2;
            }
            new_output = realloc(output, new_capacity);
            if (!new_output)
            {
                free(output);
                output = NULL;
                used = 0;
                capacity = 0;
                continue;
            }
            output = new_output;
            capacity = new_capacity;
        }
        if (output)
        {
            memcpy(output + used, chunk, (size_t)count);
            used += (size_t)count;
            output[used] = '\0';
        }
    }
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0)
    {
        free(output);
        printf("[失败] 无法取得系统命令结果：%s\n", strerror(errno));
        return -1;
    }
    if (output && used > 0 && child_exit_code(status) != 0)
    {
        printf("%s%s", output, output[used - 1] == '\n' ? "" : "\n");
    }
    free(output);
    return child_exit_code(status);
}

int run_cmd(const char *const argv[])
{
    pid_t pid;
    int status = 0;

    if (terminal_ui_enabled())
    {
        return run_cmd_in_terminal_ui(argv);
    }

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
