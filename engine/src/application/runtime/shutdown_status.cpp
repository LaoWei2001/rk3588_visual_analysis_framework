#include "shutdown_status.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace
{
std::string path, start_ticks;
auto started = std::chrono::steady_clock::now();

void publish(const char *stage, bool clean, int exit_code, unsigned long persistence_errors = 0)
{
    if (path.empty())
        return;
    const std::string temporary = path + "." + std::to_string(getpid()) + ".tmp";
    FILE *file = fopen(temporary.c_str(), "w");
    if (!file)
        return;
    // stage and start_ticks are internal constants/numbers, never user strings.
    bool ok =
        fprintf(file,
                "{\"pid\":%d,\"start_ticks\":\"%s\",\"stage\":\"%s\","
                "\"clean\":%s,\"exit_code\":%d,\"persistence_errors\":%lu}\n",
                getpid(), start_ticks.c_str(), stage, clean ? "true" : "false", exit_code, persistence_errors) > 0;
    if (fflush(file) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    if (ok && rename(temporary.c_str(), path.c_str()) == 0)
        return;
    unlink(temporary.c_str());
}
} // namespace

void shutdown_status_begin()
{
    const char *configured = std::getenv("RK_SHUTDOWN_STATUS_FILE");
    if (configured)
        path = configured;
    std::ifstream stat("/proc/self/stat");
    std::string line, field;
    std::getline(stat, line);
    const auto end = line.rfind(')');
    if (end != std::string::npos)
    {
        std::istringstream fields(line.substr(end + 1));
        for (int i = 0; i <= 19 && fields >> field; ++i)
            if (i == 19)
                start_ticks = field;
    }
    publish("running", false, 0);
}

void shutdown_status_stage(const char *stage)
{
    static bool first = true;
    if (first)
    {
        started = std::chrono::steady_clock::now();
        first = false;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    printf("[Shutdown] %.3fs: %s\n", seconds, stage);
    publish(stage, false, 0);
}

void shutdown_status_complete(int exit_code, unsigned long persistence_errors)
{
    publish("complete", exit_code == 0 && persistence_errors == 0, exit_code, persistence_errors);
}
