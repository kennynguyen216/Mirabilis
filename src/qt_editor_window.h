#pragma once
#include "editor_assets.h"
#include <QMainWindow>
#include <array>
#include <unordered_map>

class VulkanEngine;
class QWindow;
class QTreeWidget;
class QLineEdit;
class QDoubleSpinBox;
class QFileSystemModel;
class QListView;
class QLabel;
class QSortFilterProxyModel;

class QtEditorWindow final : public QMainWindow {
public:
    explicit QtEditorWindow(VulkanEngine& engine);
    ~QtEditorWindow() override;
protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    void refresh_scene();
    void request(EditorCommand command);
    void open_scene();
    void choose_directory(const QString& path);
    void update_asset_selection();
    VulkanEngine& engine_;
    QWindow* viewportWindow_{};
    QWidget* viewport_{};
    QTreeWidget* outliner_{};
    QLineEdit* name_{};
    std::array<QDoubleSpinBox*, 9> transform_{};
    QFileSystemModel* files_{};
    QSortFilterProxyModel* filter_{};
    QListView* assets_{};
    QLabel* assetInfo_{};
    QLabel* preview_{};
    QString root_, selectedAsset_, treeFingerprint_;
    uint64_t shownEpoch_{};
    EntityRef nameTarget_{};
    uint64_t nameEpoch_{};
    std::array<EntityRef, 9> fieldTargets_{};
    std::array<uint64_t, 9> fieldEpochs_{};
    std::unordered_map<PersistentID, EntityRef> shownEntities_;
};

#ifdef MIRABILIS_EDITOR_TESTS
void install_qt_browser_test_driver(QtEditorWindow&, VulkanEngine&);
#endif
