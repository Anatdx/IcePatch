#include "cli.hpp"
#include "log.hpp"

int main(int argc, char** argv) {
  apd::InitLog();
  return apd::RunCli(argc, argv);
}
