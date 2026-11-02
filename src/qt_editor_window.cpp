#include "qt_editor_window.h"
#include "qt_editor_style.h"
#include "qt_asset_delegate.h"
#include "vk_engine.h"

#include <SDL_syswm.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QMenu>
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
#include <QLocale>
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
#include <QTabWidget>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <limits>

namespace {
class AssetFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
protected:
    bool filterAcceptsRow(int row,const QModelIndex& parent) const override {
        const auto* files=qobject_cast<const QFileSystemModel*>(sourceModel());
        const auto info=files->fileInfo(files->index(row,0,parent));
        if(info.isDir())return true;
        if(QStringList{"bin","meta","tmp","pdb","spv"}.contains(info.suffix().toLower()))return false;
        return QSortFilterProxyModel::filterAcceptsRow(row,parent);
    }
};

int SDLCALL queue_close(void* owner, SDL_Event* event) {
    if (event->type == SDL_QUIT || (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE))
        QMetaObject::invokeMethod(static_cast<QtEditorWindow*>(owner), "close", Qt::QueuedConnection);
    return 1;
}
QString text(const std::string& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
std::filesystem::path path(const QString& value) { return std::filesystem::path(value.toStdWString()); }

}

QtEditorWindow::QtEditorWindow(VulkanEngine& engine) : engine_(engine) {
    setObjectName("MirabilisEditor");
    setWindowTitle("Mirabilis Editor"); resize(1600, 1000);
    setDockNestingEnabled(true);setTabPosition(Qt::AllDockWidgetAreas,QTabWidget::North);
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

    auto* toolbar = addToolBar("Scene"); toolbar->setObjectName("SceneToolbar");toolbar->setMovable(false);toolbar->setIconSize({18,18});
    const auto action = [&](const QString& title, const char* objectName, EditorCommand command) {
        auto* button = new QPushButton(title, toolbar); button->setObjectName(objectName);
        if(command==EditorCommand::Save)button->setIcon(editor_icon("Save"));
        if(command==EditorCommand::TogglePlay)button->setIcon(editor_icon("Play"));
        if(command==EditorCommand::CreateCube)button->setIcon(editor_icon("Mesh"));
        toolbar->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { request(command); });
    };
    action("Save", "SaveScene", EditorCommand::Save);
    action("Play / Edit", "TogglePlay", EditorCommand::TogglePlay);
    action("Cube", "CreateCube", EditorCommand::CreateCube);
    action("Point light", "CreatePointLight", EditorCommand::CreatePointLight);
    action("Spot light", "CreateSpotLight", EditorCommand::CreateSpotLight);
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
    const auto propertyNumber=[this](QFormLayout* layout,const QString& title,EditorCommand command,int field,double low,double high,double step){
        auto* number=new QDoubleSpinBox(this);
        number->setObjectName(QString(command==EditorCommand::SetRendering?"Rendering%1":command==EditorCommand::SetLight?"Light%1":"Material%1").arg(field));
        number->setRange(low,high);number->setDecimals(3);number->setSingleStep(step);layout->addRow(title,number);
        connect(number,&QDoubleSpinBox::editingFinished,this,[this,number,command,field]{edit_property(command,field,float(number->value()),number);});return number;
    };
    auto* material=new QGroupBox("Material",propertyWidget);auto* materialForm=new QFormLayout(material);form->addRow(material);
    materialEnabled_=new QCheckBox("Override primitive material",material);materialForm->addRow(materialEnabled_);
    twoSided_=new QCheckBox("Two-sided lighting",material);materialForm->addRow(twoSided_);
    materialCel_=new QCheckBox("Cel style",material);materialForm->addRow(materialCel_);
    const std::array<const char*,8> materialNames{"Metallic","Roughness","Specular","Thin subsurface","Transmission","IOR","Thickness","Material AO"};
    for(int i=0;i<8;++i)materialFields_[i]=propertyNumber(materialForm,materialNames[i],EditorCommand::SetMaterial,3+i,i==5?1:0,i==5?3:i==6?10:1,0.05);
    for(auto pair:{std::pair{materialEnabled_,0},std::pair{twoSided_,1},std::pair{materialCel_,2}})
        connect(pair.first,&QCheckBox::clicked,this,[this,pair](bool on){edit_property(EditorCommand::SetMaterial,pair.second,on?1.f:0.f,pair.first);});
    auto* note=new QLabel("Imported models keep their glTF materials. Use Rendering for a scene-wide cel style.",material);note->setWordWrap(true);note->setStyleSheet("color:#929292");materialForm->addRow(note);
    lightGroup_=new QGroupBox("Light",propertyWidget);auto* lightForm=new QFormLayout(lightGroup_);form->addRow(lightGroup_);
    lightType_=new QComboBox(lightGroup_);lightType_->addItems({"Point","Spot"});lightForm->addRow("Type",lightType_);
    lightEnabled_=new QCheckBox("Enabled",lightGroup_);lightShadows_=new QCheckBox("Cast shadows",lightGroup_);lightForm->addRow(lightEnabled_);lightForm->addRow(lightShadows_);
    connect(lightType_,qOverload<int>(&QComboBox::activated),this,[this](int type){edit_property(EditorCommand::SetLight,0,float(type),lightType_);});
    connect(lightEnabled_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetLight,1,on?1.f:0.f,lightEnabled_);});
    connect(lightShadows_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetLight,6,on?1.f:0.f,lightShadows_);});
    const std::array<std::pair<int,const char*>,7> lightNumbers{{{2,"Intensity"},{3,"Range"},{4,"Inner cone (deg)"},{5,"Outer cone (deg)"},{7,"Red"},{8,"Green"},{9,"Blue"}}};
    for(auto [field,title]:lightNumbers)lightFields_[field]=propertyNumber(lightForm,title,EditorCommand::SetLight,field,field==3?0.051:0,field==4||field==5?89.9:10000,field==4||field==5?1:0.1);
    auto* shadowNote=new QLabel("Shadows: first active point light and first active spot light.",lightGroup_);shadowNote->setWordWrap(true);shadowNote->setStyleSheet("color:#929292");lightForm->addRow(shadowNote);
    auto* scroll=new QScrollArea(inspector);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(propertyWidget);
    inspector->setWidget(scroll); addDockWidget(Qt::RightDockWidgetArea, inspector);
    auto* rendering=new QDockWidget("Rendering",this);rendering->setObjectName("RenderingDock");
    auto* renderingWidget=new QWidget(rendering);auto* renderingForm=new QFormLayout(renderingWidget);
    cel_=new QCheckBox("Cel shading",renderingWidget);cel_->setObjectName("CelShading");renderingForm->addRow(cel_);
    bloom_=new QCheckBox("Bloom",renderingWidget);bloom_->setObjectName("Bloom");renderingForm->addRow(bloom_);
    connect(cel_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,0,on?1.f:0.f,cel_);});
    connect(bloom_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,3,on?1.f:0.f,bloom_);});
    const std::array<std::pair<int,const char*>,5> renderNames{{{1,"Cel bands"},{2,"Cel silhouette"},{4,"Bloom strength"},{5,"Bloom threshold"},{6,"Bloom scatter"}}};
    for(auto [field,title]:renderNames)renderNumbers_[field]=propertyNumber(renderingForm,title,EditorCommand::SetRendering,field,field==1?2:0,field==1?16:field==5?100:field==4?2:1,field==1?1:0.05);
    lumen_=new QCheckBox("Lumen-inspired world GI",renderingWidget);lumen_->setObjectName("WorldGI");renderingForm->addRow(lumen_);
    connect(lumen_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,13,on?1.f:0.f,lumen_);});
    probes_=new QCheckBox("Screen probes (experimental)",renderingWidget);probes_->setObjectName("ScreenProbes");renderingForm->addRow(probes_);
    connect(probes_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,14,on?1.f:0.f,probes_);});
    reflections_=new QCheckBox("Scene reflections",renderingWidget);reflections_->setObjectName("SceneReflections");renderingForm->addRow(reflections_);
    reflections_->setToolTip("Requires ready Lumen lighting and hardware ray queries. One reflected surface interaction.");
    connect(reflections_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,11,on?1.f:0.f,reflections_);});
    renderNumbers_[12]=propertyNumber(renderingForm,"Reflection roughness limit",EditorCommand::SetRendering,12,0,1,0.05);
    meshCulling_=new QCheckBox("GPU cluster culling (experimental)",renderingWidget);renderingForm->addRow(meshCulling_);
    meshCulling_->setToolTip("Static opaque geometry in the main camera. Performance depends on scene and GPU.");
    connect(meshCulling_,&QCheckBox::clicked,this,[this](bool on){edit_property(EditorCommand::SetRendering,10,on?1.f:0.f,meshCulling_);});
    tonemap_=new QComboBox(renderingWidget);tonemap_->addItems({"ACES Filmic","Reinhard","PBR Neutral"});renderingForm->addRow("Tone mapper",tonemap_);
    giQuality_=new QComboBox(renderingWidget);giQuality_->addItems({"Reference / full","High / half","Balanced / half","Performance / half","Peak / full","Ultra / half"});renderingForm->addRow("GI quality",giQuality_);
    connect(tonemap_,qOverload<int>(&QComboBox::activated),this,[this](int value){edit_property(EditorCommand::SetRendering,7,float(value),tonemap_);});
    connect(giQuality_,qOverload<int>(&QComboBox::activated),this,[this](int value){edit_property(EditorCommand::SetRendering,8,float(value),giQuality_);});
    renderNumbers_[8]=propertyNumber(renderingForm,"Render scale",EditorCommand::SetRendering,9,0.25,1,0.05);
    auto* renderScroll=new QScrollArea(rendering);renderScroll->setWidgetResizable(true);renderScroll->setFrameShape(QFrame::NoFrame);renderScroll->setWidget(renderingWidget);rendering->setWidget(renderScroll);
    addDockWidget(Qt::RightDockWidgetArea,rendering);tabifyDockWidget(inspector,rendering);inspector->raise();views->addAction(rendering->toggleViewAction());

    auto* content = new QDockWidget("Content Browser", this); content->setObjectName("ContentBrowserDock");
    auto* contentWidget = new QWidget(content); auto* layout = new QVBoxLayout(contentWidget);
    layout->setContentsMargins(6,6,6,6);layout->setSpacing(6);
    auto* navigation=new QHBoxLayout();navigation->setSpacing(3);layout->addLayout(navigation);
    const auto nav=[&](const QString& kind,const QString& tip){auto* button=new QToolButton(contentWidget);button->setIcon(editor_icon(kind));button->setToolTip(tip);navigation->addWidget(button);return button;};
    backButton_=nav("Back","Back");forwardButton_=nav("Forward","Forward");auto* up=nav("Up","Parent folder");
    auto* crumbs=new QWidget(contentWidget);crumbs_=new QHBoxLayout(crumbs);crumbs_->setContentsMargins(0,0,0,0);crumbs_->setSpacing(0);navigation->addWidget(crumbs,1);
    connect(backButton_,&QToolButton::clicked,this,[this]{if(back_.isEmpty())return;forward_.push_back(directory_);const auto target=back_.takeLast();choose_directory(target,false);});
    connect(forwardButton_,&QToolButton::clicked,this,[this]{if(forward_.isEmpty())return;back_.push_back(directory_);const auto target=forward_.takeLast();choose_directory(target,false);});
    connect(up,&QToolButton::clicked,this,[this]{choose_directory(QFileInfo(directory_).absolutePath());});
    auto* search = new QLineEdit(contentWidget); search->setObjectName("AssetSearch"); search->setPlaceholderText("Search assets in this folder");search->setClearButtonEnabled(true);
    search->addAction(editor_icon("Search"),QLineEdit::LeadingPosition);search->setMinimumWidth(190);search->setMaximumWidth(360);navigation->addWidget(search);
    auto* splitter = new QSplitter(contentWidget); layout->addWidget(splitter);
    auto* folderPane=new QWidget(splitter);auto* folderLayout=new QVBoxLayout(folderPane);folderLayout->setContentsMargins(0,0,0,0);folderLayout->setSpacing(0);
    auto* project=new QLabel("PROJECT / CONTENT",folderPane);project->setProperty("section",true);folderLayout->addWidget(project);
    auto* folders = new QTreeView(folderPane); folders->setObjectName("AssetFolders");folders->setHeaderHidden(true);folderLayout->addWidget(folders);
    auto* folderModel = new QFileSystemModel(folders); folderModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    folderModel->setRootPath(root_); folders->setModel(folderModel); folders->setRootIndex(folderModel->index(root_));
    for (int column = 1; column < folderModel->columnCount(); ++column) folders->hideColumn(column);
    files_ = new QFileSystemModel(this); files_->setReadOnly(true); files_->setRootPath(root_);
    files_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    filter_ = new AssetFilter(this); filter_->setSourceModel(files_); filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    assets_ = new QListView(splitter); assets_->setObjectName("ContentAssets"); assets_->setModel(filter_);
    assets_->setViewMode(QListView::IconMode); assets_->setResizeMode(QListView::Adjust);
    assets_->setIconSize(QSize(64,64));assets_->setGridSize(QSize(120,124));assets_->setUniformItemSizes(true);
    assets_->setMovement(QListView::Static);assets_->setMouseTracking(true);assets_->setSpacing(0);assets_->setWordWrap(true);
    assets_->setItemDelegate(new QtAssetDelegate(assets_));
    assets_->setRootIndex(filter_->mapFromSource(files_->index(root_)));
    folderPane->setMaximumWidth(280);folderPane->setMinimumWidth(150);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 3);
    auto* details = new QWidget(splitter);details->setMinimumWidth(190);details->setMaximumWidth(240); auto* detailLayout = new QVBoxLayout(details);
    preview_ = new QLabel(details); preview_->setObjectName("AssetPreview");preview_->setMinimumSize(160,120); preview_->setAlignment(Qt::AlignCenter); detailLayout->addWidget(preview_);
    assetInfo_ = new QLabel("Select an asset", details); assetInfo_->setWordWrap(true); detailLayout->addWidget(assetInfo_);
    detailLayout->addStretch();
    auto* buttons = new QHBoxLayout(); layout->addLayout(buttons);
    assetCount_=new QLabel(contentWidget);assetCount_->setStyleSheet("color:#929292;padding:3px;");
    const auto assetButton = [&](const QString& title, const char* objectName, EditorCommand command) {
        auto* button = new QPushButton(title, contentWidget); button->setObjectName(objectName); buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { request(command); });
    };
    assetButton("Open scene", "OpenAssetScene", EditorCommand::OpenScene);
    assetButton("Import model", "ImportAssetModel", EditorCommand::ImportModel);
    assetButton("Assign base color", "AssignAssetTexture", EditorCommand::AssignBaseColor);
    assetButton("Reimport model", "ReimportAssetModel", EditorCommand::ReimportModel);
    auto* reveal = new QPushButton("Show in Explorer", contentWidget); buttons->addWidget(reveal);
    connect(reveal, &QPushButton::clicked, this, [this] { if (editor_asset_inside(root_, selectedAsset_)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(selectedAsset_).absolutePath())); });
    buttons->addStretch();buttons->addWidget(assetCount_);
    auto* size=new QSlider(Qt::Horizontal,contentWidget);size->setToolTip("Thumbnail size");size->setRange(96,180);size->setValue(120);size->setFixedWidth(100);buttons->addWidget(size);
    connect(size,&QSlider::valueChanged,this,[this](int value){assets_->setGridSize({value,124});});
    connect(search, &QLineEdit::textChanged, filter_, &QSortFilterProxyModel::setFilterFixedString);
    const auto count=[this]{if(assetCount_)assetCount_->setText(QString("%1 items").arg(filter_->rowCount(assets_->rootIndex())));};
    connect(filter_,&QAbstractItemModel::rowsInserted,this,[count]{count();});connect(filter_,&QAbstractItemModel::rowsRemoved,this,[count]{count();});
    connect(search,&QLineEdit::textChanged,this,[count]{count();});
    connect(folders, &QTreeView::clicked, this, [this, folderModel](const QModelIndex& index) { choose_directory(folderModel->filePath(index)); });
    connect(assets_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { update_asset_selection(); });
    connect(assets_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto source = filter_->mapToSource(index);
        if (files_->isDir(source)) choose_directory(files_->filePath(source));
        else if (QFileInfo(selectedAsset_).suffix().compare("json", Qt::CaseInsensitive) == 0) open_scene();
    });
    content->setWidget(contentWidget); addDockWidget(Qt::BottomDockWidgetArea, content); content->setMinimumHeight(250);
    splitter->setSizes({190,850,210});directory_=root_;refresh_browser_path();
    resizeDocks({sceneDock,inspector},{210,290},Qt::Horizontal);
    resizeDocks({content},{280},Qt::Vertical);
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
void QtEditorWindow::choose_directory(const QString& directory,bool remember) {
    if (!editor_asset_inside(root_, directory) || !QFileInfo(directory).isDir()) return;
    const auto target=QFileInfo(directory).canonicalFilePath();
    if(remember && target!=directory_){back_.push_back(directory_);if(back_.size()>64)back_.removeFirst();forward_.clear();}
    directory_=target;
    assets_->clearSelection();assets_->setCurrentIndex({});selectedAsset_.clear();preview_->clear();assetInfo_->setText("Select an asset");
    assets_->setRootIndex(filter_->mapFromSource(files_->index(directory_)));
    refresh_browser_path();
}
void QtEditorWindow::refresh_browser_path() {
    while(auto* item=crumbs_->takeAt(0)){delete item->widget();delete item;}
    const auto add=[this](const QString& title,const QString& path){auto* button=new QToolButton(this);button->setText(title);button->setToolTip(path);crumbs_->addWidget(button);connect(button,&QToolButton::clicked,this,[this,path]{choose_directory(path);});};
    add("Content",root_);QString current=root_;
    const auto relative=QDir(root_).relativeFilePath(directory_);
    if(relative!=".")for(const auto& part:relative.split('/',Qt::SkipEmptyParts)){crumbs_->addWidget(new QLabel(" / ",this));current=QDir(current).filePath(part);add(part,current);}
    crumbs_->addStretch();backButton_->setEnabled(!back_.isEmpty());forwardButton_->setEnabled(!forward_.isEmpty());
    assetCount_->setText(QString("%1 items").arg(filter_->rowCount(assets_->rootIndex())));
}
void QtEditorWindow::update_asset_selection() {
    const auto index = filter_->mapToSource(assets_->currentIndex());
    selectedAsset_ = files_->filePath(index);
    if (!editor_asset_inside(root_, selectedAsset_)) { selectedAsset_.clear(); return; }
    const QFileInfo info(selectedAsset_);
    assetInfo_->setText(info.fileName()+"\n"+asset_kind(selectedAsset_)+"\n\n"+QLocale().formattedDataSize(info.size())+"\n\n"+QDir(root_).relativeFilePath(selectedAsset_));
    preview_->setPixmap(editor_icon(asset_kind(selectedAsset_)).pixmap(64,64));
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
void QtEditorWindow::edit_property(EditorCommand command,int field,float value,QObject* widget) {
    const auto found=propertyBindings_.find(widget);if(found==propertyBindings_.end())return;
    EditorRequest request{command};request.field=field;request.value=value;request.target=found->second.first;request.epoch=found->second.second;
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
    const auto bind=[&](QWidget* widget,bool enabled){widget->setEnabled(enabled);if(!widget->hasFocus())propertyBindings_[widget]={state.selected,state.epoch};};
    const auto number=[&](QDoubleSpinBox* widget,double value,bool enabled){bind(widget,enabled);if(!widget->hasFocus()){QSignalBlocker block(widget);widget->setValue(value);}};
    const auto check=[&](QCheckBox* widget,bool value,bool enabled){bind(widget,enabled);QSignalBlocker block(widget);widget->setChecked(value);};
    check(meshCulling_,state.rendering[10]>0.5f,state.editing);
    check(reflections_,state.rendering[11]>0.5f,state.editing);
    check(lumen_,state.rendering[13]>0.5f,state.editing);
    check(probes_,state.rendering[14]>0.5f,state.editing);
    number(renderNumbers_[12],state.rendering[12],state.editing);
    check(cel_,state.rendering[0]>0.5f,state.editing);check(bloom_,state.rendering[3]>0.5f,state.editing);
    for(int field:{1,2,4,5,6})number(renderNumbers_[field],state.rendering[field],state.editing);
    number(renderNumbers_[8],state.rendering[9],state.editing);
    bind(tonemap_,state.editing);bind(giQuality_,state.editing);
    {QSignalBlocker block(tonemap_);tonemap_->setCurrentIndex(int(state.rendering[7]));}
    {QSignalBlocker block(giQuality_);giQuality_->setCurrentIndex(int(state.rendering[8]));}
    materialEnabled_->parentWidget()->setVisible(state.material.has_value()&&!state.importedModel);
    const auto material=state.material.value_or(SceneMaterial{});const bool canMaterial=state.editing&&state.material.has_value()&&!state.importedModel;
    check(materialEnabled_,material.enabled,canMaterial);check(twoSided_,material.doubleSided,canMaterial&&material.enabled);check(materialCel_,material.celShading,canMaterial&&material.enabled);
    const std::array<float,8> materialValues{material.metallic,material.roughness,material.specular,material.subsurface,material.transmission,material.ior,material.thickness,material.occlusionStrength};
    for(int i=0;i<8;++i)number(materialFields_[i],materialValues[i],canMaterial&&material.enabled);
    lightGroup_->setVisible(state.light.has_value());
    const auto light=state.light.value_or(Light{});const bool canLight=state.editing&&state.light.has_value();
    bind(lightType_,canLight);{QSignalBlocker block(lightType_);lightType_->setCurrentIndex(int(light.type));}
    check(lightEnabled_,light.enabled,canLight);check(lightShadows_,light.castShadows,canLight);
    const std::array<float,10> lightValues{0,0,light.intensity,light.range,glm::degrees(light.innerCone),glm::degrees(light.outerCone),0,light.color.r,light.color.g,light.color.b};
    for(int i:{2,3,4,5,7,8,9})number(lightFields_[i],lightValues[i],canLight&&(i!=4&&i!=5||light.type==LightType::Spot));
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
