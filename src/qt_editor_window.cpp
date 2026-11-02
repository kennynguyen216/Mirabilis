#include "qt_editor_window.h"
#include "vk_engine.h"

#include <SDL_syswm.h>
#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QTreeWidgetItemIterator>
#include <stdexcept>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <limits>

namespace {
int SDLCALL queue_close(void* owner, SDL_Event* event) {
    if (event->type == SDL_QUIT || (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE))
        QMetaObject::invokeMethod(static_cast<QtEditorWindow*>(owner), "close", Qt::QueuedConnection);
    return 1;
}
QString text(const std::string& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
std::filesystem::path path(const QString& value) { return std::filesystem::path(value.toStdWString()); }
bool inside(const QString& root, const QString& candidate) {
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(canonical);
    return !canonical.isEmpty() && !relative.startsWith("../") && relative != ".." && !QDir::isAbsolutePath(relative);
}
}

QtEditorWindow::QtEditorWindow(VulkanEngine& engine) : engine_(engine) {
    setObjectName("MirabilisEditor");
    setWindowTitle("Mirabilis Editor"); resize(1450, 900);
    root_ = QString::fromStdWString(engine_.editor_asset_root().wstring());

    SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(engine_._window, &info)) throw std::runtime_error("Could not obtain the Vulkan viewport window");
#ifdef _WIN32
    viewportWindow_ = QWindow::fromWinId(reinterpret_cast<WId>(info.info.win.window));
#else
    throw std::runtime_error("The Qt viewport currently supports Windows");
#endif
    if (!viewportWindow_) throw std::runtime_error("Qt could not embed the Vulkan viewport");
    viewport_ = QWidget::createWindowContainer(viewportWindow_, this);
    viewport_->setObjectName("VulkanViewport"); viewport_->setFocusPolicy(Qt::StrongFocus);
    setCentralWidget(viewport_);

    auto* toolbar = addToolBar("Scene"); toolbar->setObjectName("SceneToolbar");
    const auto action = [&](const QString& title, const char* objectName, EditorCommand command) {
        auto* button = new QPushButton(title, toolbar); button->setObjectName(objectName);
        toolbar->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { request(command); });
    };
    action("Save", "SaveScene", EditorCommand::Save);
    action("Play / Edit", "TogglePlay", EditorCommand::TogglePlay);
    action("Create cube", "CreateCube", EditorCommand::CreateCube);
    action("Duplicate", "DuplicateEntity", EditorCommand::Duplicate);
    action("Delete", "DeleteEntity", EditorCommand::Delete);
    auto* views = menuBar()->addMenu("View");
    auto* debug = menuBar()->addMenu("Debug");
    debug->addAction("ImGui diagnostics", this, [this] { request(EditorCommand::ToggleDebug); });

    auto* sceneDock = new QDockWidget("Scene", this); sceneDock->setObjectName("SceneDock");
    outliner_ = new QTreeWidget(sceneDock); outliner_->setObjectName("SceneOutliner");
    outliner_->setHeaderLabel("Entities"); sceneDock->setWidget(outliner_); addDockWidget(Qt::LeftDockWidgetArea, sceneDock);
    connect(outliner_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!item) return;
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        if (auto found = shownEntities_.find(id); found != shownEntities_.end()) {
            EditorRequest request{EditorCommand::Select}; request.target = found->second; request.epoch = shownEpoch_;
            engine_.queue_editor_request(std::move(request));
        }
    });
    auto* inspector = new QDockWidget("Properties", this); inspector->setObjectName("PropertiesDock");
    auto* propertyWidget = new QWidget(inspector); auto* form = new QFormLayout(propertyWidget);
    name_ = new QLineEdit(propertyWidget); name_->setObjectName("EntityName"); form->addRow("Name", name_);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        EditorRequest request{EditorCommand::SetName}; request.target = nameTarget_; request.epoch = nameEpoch_;
        request.text = name_->text().toUtf8().toStdString(); engine_.queue_editor_request(std::move(request));
    });
    const std::array<const char*, 9> names{"Position X", "Position Y", "Position Z", "Rotation X", "Rotation Y", "Rotation Z", "Scale X", "Scale Y", "Scale Z"};
    for (int i = 0; i < 9; ++i) {
        auto* field = new QDoubleSpinBox(propertyWidget); transform_[i] = field;
        field->setObjectName(QString("Transform%1").arg(i)); field->setDecimals(6);
        field->setRange(-double(std::numeric_limits<float>::max()), double(std::numeric_limits<float>::max()));
        field->setSingleStep(i >= 3 && i < 6 ? 1.0 : 0.1); form->addRow(names[i], field);
        connect(field, &QDoubleSpinBox::editingFinished, this, [this, i, field] {
            EditorRequest request{EditorCommand::SetTransform}; request.target = fieldTargets_[i]; request.epoch = fieldEpochs_[i];
            request.field = i; request.value = float(field->value()); engine_.queue_editor_request(std::move(request));
        });
    }
    inspector->setWidget(propertyWidget); addDockWidget(Qt::RightDockWidgetArea, inspector);

    auto* content = new QDockWidget("Content Browser", this); content->setObjectName("ContentBrowserDock");
    auto* contentWidget = new QWidget(content); auto* layout = new QVBoxLayout(contentWidget);
    auto* search = new QLineEdit(contentWidget); search->setObjectName("AssetSearch"); search->setPlaceholderText("Filter this folder"); layout->addWidget(search);
    auto* splitter = new QSplitter(contentWidget); layout->addWidget(splitter);
    auto* folders = new QTreeView(splitter); folders->setObjectName("AssetFolders");
    auto* folderModel = new QFileSystemModel(folders); folderModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    folderModel->setRootPath(root_); folders->setModel(folderModel); folders->setRootIndex(folderModel->index(root_));
    for (int column = 1; column < folderModel->columnCount(); ++column) folders->hideColumn(column);
    files_ = new QFileSystemModel(this); files_->setReadOnly(true); files_->setRootPath(root_);
    files_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    filter_ = new QSortFilterProxyModel(this); filter_->setSourceModel(files_); filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    assets_ = new QListView(splitter); assets_->setObjectName("ContentAssets"); assets_->setModel(filter_);
    assets_->setViewMode(QListView::IconMode); assets_->setResizeMode(QListView::Adjust);
    assets_->setIconSize(QSize(40, 40)); assets_->setGridSize(QSize(125, 70)); assets_->setUniformItemSizes(true);
    assets_->setRootIndex(filter_->mapFromSource(files_->index(root_)));
    folders->setMaximumWidth(300);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 3);
    auto* details = new QWidget(splitter); auto* detailLayout = new QVBoxLayout(details);
    preview_ = new QLabel(details); preview_->setMinimumSize(128, 96); preview_->setAlignment(Qt::AlignCenter); detailLayout->addWidget(preview_);
    assetInfo_ = new QLabel("Select an asset", details); assetInfo_->setWordWrap(true); detailLayout->addWidget(assetInfo_);
    auto* buttons = new QHBoxLayout(); layout->addLayout(buttons);
    const auto assetButton = [&](const QString& title, const char* objectName, EditorCommand command) {
        auto* button = new QPushButton(title, contentWidget); button->setObjectName(objectName); buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { request(command); });
    };
    assetButton("Open scene", "OpenAssetScene", EditorCommand::OpenScene);
    assetButton("Import model", "ImportAssetModel", EditorCommand::ImportModel);
    assetButton("Assign base color", "AssignAssetTexture", EditorCommand::AssignBaseColor);
    assetButton("Reimport model", "ReimportAssetModel", EditorCommand::ReimportModel);
    auto* reveal = new QPushButton("Show in Explorer", contentWidget); buttons->addWidget(reveal);
    connect(reveal, &QPushButton::clicked, this, [this] { if (inside(root_, selectedAsset_)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(selectedAsset_).absolutePath())); });
    connect(search, &QLineEdit::textChanged, filter_, &QSortFilterProxyModel::setFilterFixedString);
    connect(folders, &QTreeView::clicked, this, [this, folderModel](const QModelIndex& index) { choose_directory(folderModel->filePath(index)); });
    connect(assets_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { update_asset_selection(); });
    connect(assets_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto source = filter_->mapToSource(index);
        if (files_->isDir(source)) choose_directory(files_->filePath(source));
        else if (QFileInfo(selectedAsset_).suffix().compare("json", Qt::CaseInsensitive) == 0) open_scene();
    });
    content->setWidget(contentWidget); addDockWidget(Qt::BottomDockWidgetArea, content); content->setMinimumHeight(220);
    for (auto* dock : {sceneDock, inspector, content}) views->addAction(dock->toggleViewAction());
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* next) {
        if (next && next != viewport_ && !viewport_->isAncestorOf(next)) engine_.release_editor_input();
    });
    auto* refresh = new QTimer(this); refresh->setInterval(200);
    connect(refresh, &QTimer::timeout, this, [this] { refresh_scene(); }); refresh->start();
    refresh_scene();
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this] { request(EditorCommand::Save); });
    SDL_AddEventWatch(&queue_close, this);
    if (!engine_.editor_snapshot().editing) request(EditorCommand::TogglePlay);
    engine_.release_editor_input();
}

QtEditorWindow::~QtEditorWindow() {
    SDL_DelEventWatch(&queue_close, this);
    // Release Qt's wrapper ownership before SDL destroys the native Vulkan window.
    if (viewportWindow_) { viewportWindow_->setParent(nullptr); delete viewportWindow_; viewportWindow_ = nullptr; }
}
void QtEditorWindow::choose_directory(const QString& directory) {
    if (!inside(root_, directory) || !QFileInfo(directory).isDir()) return;
    assets_->setRootIndex(filter_->mapFromSource(files_->index(directory)));
}
void QtEditorWindow::update_asset_selection() {
    const auto index = filter_->mapToSource(assets_->currentIndex());
    selectedAsset_ = files_->filePath(index);
    if (!inside(root_, selectedAsset_)) { selectedAsset_.clear(); return; }
    const QFileInfo info(selectedAsset_);
    assetInfo_->setText(QDir(root_).relativeFilePath(selectedAsset_) + "\n" + QString::number(info.size()) + " bytes");
    preview_->clear();
    QImageReader reader(selectedAsset_);
    const QSize size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() <= 16 * 1024 * 1024) {
        reader.setScaledSize(size.scaled(192, 128, Qt::KeepAspectRatio));
        const auto image = reader.read(); if (!image.isNull()) preview_->setPixmap(QPixmap::fromImage(image));
    }
}
void QtEditorWindow::open_scene() {
    bool discard = false;
    if (engine_.editor_snapshot().dirty) {
        const auto choice = QMessageBox::question(this, "Unsaved scene", "Save changes before opening another scene?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (choice == QMessageBox::Cancel) return;
        if (choice == QMessageBox::Save) { if (!engine_.save_from_editor()) return; }
        else discard = true;
    }
    EditorRequest request{EditorCommand::OpenScene}; request.path = path(selectedAsset_); request.discard = discard;
    engine_.queue_editor_request(std::move(request));
}
void QtEditorWindow::request(EditorCommand command) {
    if (command == EditorCommand::OpenScene) { open_scene(); return; }
    EditorRequest request{command};
    if (command == EditorCommand::ImportModel || command == EditorCommand::AssignBaseColor || command == EditorCommand::ReimportModel)
        request.path = path(selectedAsset_);
    engine_.queue_editor_request(std::move(request));
}
void QtEditorWindow::refresh_scene() {
    const auto state = engine_.editor_snapshot();
    QString fingerprint = QString::number(state.epoch);
    for (const auto& row : state.entities) fingerprint += QString("|%1:%2:%3").arg(row.reference.persistentId).arg(row.parent.value_or(UINT64_MAX)).arg(text(row.name));
    if (fingerprint != treeFingerprint_) {
        QSignalBlocker block(outliner_); outliner_->clear(); shownEntities_.clear();
        std::unordered_map<PersistentID, QTreeWidgetItem*> items;
        for (const auto& row : state.entities) {
            auto* item = new QTreeWidgetItem(QStringList{text(row.name)});
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(row.reference.persistentId));
            if (row.parent && items.contains(*row.parent)) items[*row.parent]->addChild(item);
            else outliner_->addTopLevelItem(item);
            items[row.reference.persistentId] = item; shownEntities_[row.reference.persistentId] = row.reference;
        }
        outliner_->expandAll(); treeFingerprint_ = fingerprint;
    }
    {
        QSignalBlocker block(outliner_);
        if (!state.selected) outliner_->setCurrentItem(nullptr);
        for (QTreeWidgetItemIterator iterator(outliner_); *iterator; ++iterator)
            if (state.selected && (*iterator)->data(0, Qt::UserRole).toULongLong() == state.selected.persistentId) outliner_->setCurrentItem(*iterator);
    }
    shownEpoch_ = state.epoch;
    if (!name_->hasFocus()) { QSignalBlocker block(name_); name_->setText(text(state.selectedName)); nameTarget_ = state.selected; nameEpoch_ = state.epoch; }
    name_->setEnabled(state.editing && bool(state.selected));
    for (int i = 0; i < 9; ++i) {
        QSignalBlocker block(transform_[i]);
        if (!transform_[i]->hasFocus()) { transform_[i]->setValue(state.transform[i]); fieldTargets_[i] = state.selected; fieldEpochs_[i] = state.epoch; }
        transform_[i]->setEnabled(state.editing && bool(state.selected));
    }
    setWindowTitle(text(state.scene) + (state.dirty ? " *" : "") + " — Mirabilis Editor");
    statusBar()->showMessage((state.editing ? "Edit  |  " : "Play  |  ") + text(state.status));
}
void QtEditorWindow::closeEvent(QCloseEvent* event) {
    if (engine_.editor_snapshot().dirty) {
        const auto choice = QMessageBox::question(this, "Unsaved scene", "Save before closing?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (choice == QMessageBox::Cancel || (choice == QMessageBox::Save && !engine_.save_from_editor())) { event->ignore(); return; }
        engine_._editorDiscardOnExit = choice == QMessageBox::Discard;
    }
    engine_.request_editor_quit(); event->accept();
}
void QtEditorWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) engine_.stop_rendering = isMinimized();
}
