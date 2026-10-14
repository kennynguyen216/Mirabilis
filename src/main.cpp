
#include <vk_engine.h>
#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::set_terminate([] {
        try {
            if (auto failure = std::current_exception()) std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Unhandled renderer exception: %s\n", error.what());
        } catch (...) {
            std::fputs("Unhandled renderer exception of unknown type\n", stderr);
        }
        std::abort();
    });
#ifdef _MSC_VER
    if (std::getenv("MIRABILIS_TEST_FRAMES")) _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    VulkanEngine engine;

    engine.init();

    engine.run();

    engine.cleanup();

    return 0;
}
