// Exercise the real SDK implementation against a fake controller. No GPIO device is opened.
#include <atomic>
#include <cassert>
#include <functional>
#include <poll.h>
#include <sys/un.h>
#include <thread>

static char test_state_dir[128];
static char test_runtime_dir[128];
static char test_persistence_file[128];
static char test_control_socket[sizeof(sockaddr_un::sun_path)];
#define GPIO_STATE_DIR test_state_dir
#define GPIO_RUNTIME_DIR test_runtime_dir
#define GPIO_PERSISTENCE_ENABLE_FILE test_persistence_file
#define GPIO_CONTROL_SOCKET test_control_socket
#include "../../src/platform/gpio/gpio.cpp"

static int hardware_attempts = 0;
extern "C" struct gpiod_chip *__wrap_gpiod_chip_open_by_name(const char *)
{
    ++hardware_attempts;
    errno = ENODEV;
    return nullptr;
}
extern "C" struct gpiod_chip *__wrap_gpiod_chip_open_by_number(unsigned int)
{
    ++hardware_attempts;
    errno = ENODEV;
    return nullptr;
}

class Controller
{
  public:
    explicit Controller(std::string response) : response_(std::move(response))
    {
        listener_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        assert(listener_ >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", test_control_socket);
        assert(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        assert(listen(listener_, 4) == 0);
        thread_ = std::thread([this] {
            while (!stop_.load())
            {
                pollfd pending{listener_, POLLIN, 0};
                if (poll(&pending, 1, 20) <= 0)
                    continue;
                int client = accept(listener_, nullptr, nullptr);
                assert(client >= 0);
                char request[128]{};
                const auto length = recv(client, request, sizeof(request) - 1, 0);
                assert(length > 0);
                requests.emplace_back(request, static_cast<std::size_t>(length));
                assert(send(client, response_.data(), response_.size(), MSG_NOSIGNAL) ==
                       static_cast<ssize_t>(response_.size()));
                close(client);
            }
        });
    }
    void finish()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
    }
    ~Controller()
    {
        finish();
        close(listener_);
        unlink(test_control_socket);
    }
    std::vector<std::string> requests;

  private:
    int listener_ = -1;
    std::string response_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

static void check_request(const std::string &operation, const std::string &response, const std::function<int()> &call,
                          int expected, int expected_errno = 0)
{
    gpio_deinit();
    hardware_attempts = 0;
    Controller controller(response);
    errno = 0;
    const int result = call();
    const int error = errno;
    controller.finish();
    assert(result == expected);
    if (expected_errno)
        assert(error == expected_errno);
    assert(controller.requests.size() == 1 && controller.requests[0] == operation);
    assert(hardware_attempts == 0); // Protocol failures must not trigger local access or old commands.
}

int main()
{
    char temporary[] = "/tmp/vision-gpio-protocol-XXXXXX";
    const char *directory = mkdtemp(temporary);
    assert(directory);
    std::snprintf(test_state_dir, sizeof(test_state_dir), "%s/state", directory);
    std::snprintf(test_runtime_dir, sizeof(test_runtime_dir), "%s/runtime", directory);
    std::snprintf(test_persistence_file, sizeof(test_persistence_file), "%s/enabled", directory);
    std::snprintf(test_control_socket, sizeof(test_control_socket), "%s/control.sock", directory);
    const auto output = [] { return gpio_set_output("GPIO6_A0", 1); };
    const auto input = [] { return gpio_set_input("GPIO6_B3"); };
    int value = -1;
    const auto read = [&] { return gpio_read_input("GPIO6_B3", &value); };
    check_request("SET_MANAGED GPIO6_A0 1", "OK 1", output, 0);
    check_request("INPUT_MANAGED GPIO6_B3", "OK 0", input, 0);
    check_request("READ GPIO6_B3", "OK 1", read, 0);
    assert(value == 1);
    for (int error : {EINVAL, EPROTO, EBUSY})
    {
        const auto response = "ERR " + std::to_string(error);
        check_request("SET_MANAGED GPIO6_A0 1", response, output, -2, error);
        check_request("INPUT_MANAGED GPIO6_B3", response, input, -2, error);
        check_request("READ GPIO6_B3", response, read, -1, error);
    }
    check_request("SET_MANAGED GPIO6_A0 1", "invalid response", output, -2, EPROTO);
    check_request("SET_MANAGED GPIO6_A0 1", "OK 0", output, -2, EIO);
    check_request("SET_MANAGED GPIO6_A0 1", "APPLIED 1 " + std::to_string(EIO), output, -3, EIO);
    check_request("INPUT_MANAGED GPIO6_B3", "APPLIED 0 " + std::to_string(EIO), input, -3, EIO);
    GPIOOutputStatus_t status{};
    check_request(
        "STATUS GPIO6_A0", "OK 1", [&] { return gpio_get_output_status("GPIO6_A0", &status); }, 0);
    assert(status.hasCurrentValue && status.currentValue == 1);
    check_request(
        "STATUS GPIO6_A0", "ERR " + std::to_string(EPROTO), [&] { return gpio_get_output_status("GPIO6_A0", &status); },
        -1, EPROTO);
    gpio_deinit();
    hardware_attempts = 0;
    assert(output() < 0 && hardware_attempts == 1); // No service: the local backend is still selected.
    gpio_deinit();
    assert(rmdir(directory) == 0);
    std::puts("GPIO SDK protocol, error propagation and local backend selection passed");
}
