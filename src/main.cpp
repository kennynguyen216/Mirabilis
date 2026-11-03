
#include <vk_engine.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#ifdef MIRABILIS_QT_EDITOR
#include "qt_editor_window.h"
#include "qt_editor_style.h"
#include <QApplication>
#include <QEventLoop>
#include <QPalette>
#include <QStyleFactory>
#endif

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
#ifdef MIRABILIS_QT_EDITOR
    const char* qt = std::getenv("MIRABILIS_QT_EDITOR");
    const bool useQt = qt ? std::string_view(qt) != "0" : !std::getenv("MIRABILIS_TEST_FRAMES");
    std::unique_ptr<QApplication> app;
    if (useQt) {
        app = std::make_unique<QApplication>(argc, argv);
        apply_editor_style(*app);
    }
#endif
    VulkanEngine engine;

    engine.init();

#ifdef MIRABILIS_QT_EDITOR
    std::unique_ptr<QtEditorWindow> editor;
    if (app) {
        engine._qtEditorActive = true;
        editor = std::make_unique<QtEditorWindow>(engine);
        editor->show();
        engine._editorHostPump = [&] {
            app->processEvents(QEventLoop::AllEvents, 2);
            const auto* viewport = editor->centralWidget();
            engine.stop_rendering = editor->isMinimized() || viewport->width() <= 0 || viewport->height() <= 0;
        };
#ifdef MIRABILIS_EDITOR_TESTS
        if (std::getenv("MIRABILIS_TEST_QT_BROWSER")) install_qt_browser_test_driver(*editor, engine);
#endif
    }
#endif
    engine.run();
#ifdef MIRABILIS_QT_EDITOR
    engine._editorHostPump = {};
    editor.reset();
#endif

    engine.cleanup();

    return 0;
}
