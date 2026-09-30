  #include "gpio/gpio.h"

  #include <cstdio>
  #include <cstring>

  int main(int argc, char **argv)
  {
      if (argc != 2 ||
          (std::strcmp(argv[1], "0") != 0 &&
           std::strcmp(argv[1], "1") != 0)) {
          std::printf("用法：%s <0|1>\n", argv[0]);
          return 1;
      }

      return gpio_set_output("GPIO6_A2", argv[1][0] - '0');
  }
