
#include <vk_engine.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _MSC_VER
    if (std::getenv("MIRABILIS_TEST_FRAMES")) _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    VulkanEngine engine;

    engine.init();

    engine.run();

    engine.cleanup();

    return 0;
}
