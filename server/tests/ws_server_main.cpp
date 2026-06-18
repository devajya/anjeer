#include <catch2/catch_session.hpp>
#include <unistd.h>

// Detached uWS event-loop threads outlive the test binary and trigger a
// use-after-free when normal exit destroys static singletons under them.
// _exit() skips static destructors so the OS reaps the threads cleanly.
int main(int argc, char* argv[]) {
    int result = Catch::Session().run(argc, argv);
    _exit(result);
}
