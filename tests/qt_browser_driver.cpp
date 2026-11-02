#include "qt_editor_window.h"
#include "vk_engine.h"
#include "imgui_internal.h"
#include <QAction>
#include <QTimer>
#include <QApplication>
#include <QMessageBox>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QListView>
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QImage>
#include <QDir>
#include <cstdio>
#include <cmath>

// Runs through the real Qt widgets and owner-thread request queue, using disposable assets.
void install_qt_browser_test_driver(QtEditorWindow& window, VulkanEngine& engine) {
    struct State { int graphicsStage{}; int stage{}; uint64_t epoch{}; EntityRef imported{}; QElapsedTimer elapsed; };
    auto state = std::make_shared<State>(); state->elapsed.start();
    auto* timer = new QTimer(&window); timer->setInterval(300);
    QObject::connect(timer, &QTimer::timeout, &window, [&, state, timer] {
        const auto require = [](bool condition, const char* message) {
            if (!condition) { std::fprintf(stderr, "QT TEST FAIL: %s\n", message); std::abort(); }
        };
        require(state->elapsed.elapsed() < 90000, "editor action timed out");
        const auto answerClose = [&](QMessageBox::StandardButton button) {
            auto* answer = new QTimer(&window); answer->setInterval(10);
            QObject::connect(answer, &QTimer::timeout, &window, [answer, button] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    answer->stop(); box->button(button)->click(); answer->deleteLater();
                }
            });
            answer->start();
        };
        const auto escape = [] {
            SDL_Event quit{}; quit.type = SDL_KEYDOWN; quit.key.keysym.sym = SDLK_ESCAPE; SDL_PushEvent(&quit);
        };
        const auto snapshot = engine.editor_snapshot();
        auto* view = window.findChild<QListView*>("ContentAssets");
        auto* proxy = qobject_cast<QSortFilterProxyModel*>(view->model());
        auto* files = qobject_cast<QFileSystemModel*>(proxy->sourceModel());
        const auto select = [&](const QString& name) {
            const auto source = files->index(QString::fromStdWString(engine.editor_asset_root().wstring()) + "/" + name);
            const auto index = proxy->mapFromSource(source);
            if (!index.isValid()) return false;
            view->setRootIndex(proxy->mapFromSource(files->index(QFileInfo(files->filePath(source)).absolutePath())));
            view->setCurrentIndex(index); return true;
        };
        const auto click = [&](const char* name) {
            auto* button = window.findChild<QPushButton*>(name); require(button, "missing editor button"); button->click();
        };
        if(state->stage==0 && state->graphicsStage<6) {
            switch(state->graphicsStage) {
            case 0: {
                if(!snapshot.editing)return;
                auto* cel=window.findChild<QCheckBox*>("CelShading");auto* bloom=window.findChild<QCheckBox*>("Bloom");
                require(cel&&bloom,"missing Qt rendering controls");cel->click();bloom->click();click("CreatePointLight");break;
            }
            case 1: {
                if(!snapshot.light)return;
                require(snapshot.rendering[0]>0.5f&&snapshot.rendering[3]>0.5f,"Qt cel/bloom edit did not reach renderer");
                auto* range=window.findChild<QDoubleSpinBox*>("Light3");require(range,"missing Qt light inspector");
                range->setValue(8.5);QMetaObject::invokeMethod(range,"editingFinished");break;
            }
            case 2:
                if(!snapshot.light||std::abs(snapshot.light->range-8.5f)>1e-6f)return;
                click("TogglePlay");break;
            case 3: {
                if(snapshot.editing)return;
                EditorRequest request{EditorCommand::SetLight};request.field=3;request.value=99;engine.queue_editor_request(std::move(request));break;
            }
            case 4:
                require(snapshot.status=="Return to Edit mode to change authored content","Play allowed authored light mutation");click("TogglePlay");break;
            case 5:
                if(!snapshot.editing)return;
                require(snapshot.light&&std::abs(snapshot.light->range-8.5f)<1e-6f,"Play did not restore authored light");
                click("DeleteEntity");break;
            }
            ++state->graphicsStage;return;
        }
        switch (state->stage) {
        case 0:
            if (!snapshot.editing) { click("TogglePlay"); return; }
            if (!select("triangle.gltf")) return;
            click("ImportAssetModel"); break;
        case 1:
            if (snapshot.status != "Model imported") return;
            require(snapshot.selected && snapshot.selectedName.find("triangle") != std::string::npos, "model was not selected");
            state->imported = snapshot.selected;
            if (!select("color.png")) return;
            click("AssignAssetTexture"); break;
        case 2:
            if (snapshot.status != "Base color assigned") return;
            if (!select("broken.png")) return;
            click("AssignAssetTexture"); break; // allow the properties refresh to bind the newly selected entity
        case 3: {
            require(snapshot.status == "Texture could not be decoded; material retained", "bad texture replaced working material");
            auto* field = window.findChild<QDoubleSpinBox*>("Transform0"); require(field, "missing property editor");
            field->setValue(2.75); QMetaObject::invokeMethod(field, "editingFinished"); break;
        }
        case 4:
            if (snapshot.status != "Property updated") return;
            require(std::abs(snapshot.transform[0] - 2.75f) < 1e-6f, "transform edit missed selected entity");
            click("SaveScene"); break;
        case 5:
            if (snapshot.status != "Scene saved") return;
            require(!snapshot.dirty, "save left scene dirty"); state->epoch = snapshot.epoch;
            if (!select("scenes/sandbox.json")) return;
            click("OpenAssetScene"); break;
        case 6:
            if (snapshot.epoch == state->epoch) return;
            require(snapshot.status == "Scene opened", "scene did not reopen");
            require(snapshot.entities.size() > 1, "scene lost its entities");
            // Old identity cannot edit a replacement scene, even if its persistent ID is reused.
            { EditorRequest stale{EditorCommand::SetName}; stale.target = state->imported; stale.epoch = state->epoch; stale.text = "STALE";
              engine.queue_editor_request(std::move(stale)); }
            break;
        case 7:
            require(snapshot.status == "Scene changed; retry the action", "stale scene action was accepted");
            if (!select("triangle.gltf")) return;
            click("ReimportAssetModel"); break;
        case 8:
            if (!snapshot.status.starts_with("Reimported model")) return;
            { auto* dock = window.findChild<QDockWidget*>("ContentBrowserDock"); require(dock, "missing content dock"); dock->setFloating(true); }
            window.resize(1200, 820); break;
        case 9: {
            EditorRequest outside{EditorCommand::ImportModel}; outside.path = engine.editor_asset_root().parent_path() / "outside.gltf";
            engine.queue_editor_request(std::move(outside));
            auto* dock = window.findChild<QDockWidget*>("ContentBrowserDock"); require(dock->isFloating(), "dock did not float");
            dock->toggleViewAction()->trigger(); require(!dock->isVisible(), "content dock did not hide");
            dock->toggleViewAction()->trigger(); require(dock->isVisible(), "content dock did not reopen");
            dock->setFloating(false); window.addDockWidget(Qt::BottomDockWidgetArea, dock);
            window.showMinimized(); break;
        }
        case 10:
            require(snapshot.status == "Choose an existing file inside the asset root", "out-of-root asset was accepted");
            require(window.isMinimized(), "window did not minimize"); window.showNormal(); break;
        case 11:
            require(!engine.stop_rendering, "renderer stayed stopped after restore");
            if (const char* capture = std::getenv("MIRABILIS_TEST_QT_CAPTURE")) require(window.grab().save(QString::fromUtf8(capture)), "editor capture failed");
            if (const char* capture = std::getenv("MIRABILIS_TEST_QT_RASTER")) engine.capture_raster(capture);
            require(engine.tasks().statistics().completed > 0, "loader never used task system");
            { bool found = false;
              for (auto* action : window.findChildren<QAction*>()) if (action->text() == "ImGui diagnostics") { action->trigger(); found = true; break; }
              require(found, "missing ImGui debug action"); }
            break;
        case 12:
            if (!ImGui::FindWindowByName("Profiler") || !ImGui::FindWindowByName("Profiler")->Active) {
                std::printf("QT waiting for profiler: frame=%d stopped=%d\n", engine._frameNumber, int(engine.stop_rendering)); return;
            }
            click("TogglePlay"); break;
        case 13:
            require(!snapshot.editing, "Play did not start");
            require(ImGui::FindWindowByName("Profiler") && ImGui::FindWindowByName("Profiler")->Active, "ImGui profiler did not render in Play");
            if (!select("triangle.gltf")) return;
            click("ImportAssetModel"); break;
        case 14:
            require(snapshot.status == "Return to Edit mode to change authored content", "Play allowed authored asset mutation");
            click("TogglePlay"); break;
        case 15:
            require(snapshot.editing, "Edit did not restore"); click("CreateCube"); break;
        case 16:
            require(snapshot.dirty, "created cube did not dirty the scene");
            answerClose(QMessageBox::Cancel); escape(); break;
        case 17:
            require(window.isVisible() && snapshot.dirty && !engine._editorQuitRequested, "Cancel closed the editor or lost edits");
            std::puts("QT BROWSER PASS: import, texture failure, transform, save/reopen, stale identity, reimport, dock, resize, minimize/restore, ImGui Edit/Play, Play asset protection, close cancel/discard");
            timer->stop(); answerClose(QMessageBox::Discard); escape(); return;
        }
        ++state->stage;
    });
    QObject::connect(&window, &QObject::destroyed, [state, &engine] {
        if (state->stage != 17 || !engine._editorDiscardOnExit) {
            std::fputs("QT TEST FAIL: close did not complete explicit Discard\n", stderr); std::abort();
        }
    });
    timer->start();
}
