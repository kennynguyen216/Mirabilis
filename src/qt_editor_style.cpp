#include "qt_editor_style.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>

QString asset_kind(const QString &path) {
    QFileInfo file(path);
    if (file.isDir())
        return "Folder";
    const auto ext = file.suffix().toLower();
    if (QStringList{"png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds", "ktx", "ktx2"}
            .contains(ext))
        return "Texture";
    if (ext == "glb" || ext == "gltf" || ext == "obj" || ext == "fbx")
        return "Mesh";
    if (ext == "json")
        return "Scene";
    if (ext == "cs" || ext == "cpp" || ext == "h" || ext == "glsl")
        return "Script";
    return "Asset";
}

QIcon editor_icon(const QString &kind) {
    static QHash<QString, QIcon> cache;
    if (auto found = cache.constFind(kind); found != cache.cend())
        return *found;
    // Vector-painted icons stay sharp without an external icon pack or asset dependency.
    QPixmap image(96, 96);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor color = kind == "Folder"    ? QColor("#d7ae62")
                         : kind == "Texture" ? QColor("#bd86d4")
                         : kind == "Mesh"    ? QColor("#75b6d6")
                         : kind == "Scene"   ? QColor("#7fc19b")
                                             : QColor("#b6bbc5");
    p.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(color.red(), color.green(), color.blue(), 35));
    if (kind == "Folder") {
        QPainterPath path;
        path.moveTo(15, 29);
        path.lineTo(39, 29);
        path.lineTo(46, 36);
        path.lineTo(80, 36);
        path.lineTo(77, 72);
        path.lineTo(15, 72);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(16, 43, 78, 43);
    } else if (kind == "Mesh" || kind == "Cube") {
        QPolygonF top{{48, 19}, {76, 34}, {48, 50}, {20, 34}},
            left{{20, 34}, {48, 50}, {48, 80}, {20, 64}},
            right{{48, 50}, {76, 34}, {76, 64}, {48, 80}};
        p.drawPolygon(top);
        p.drawPolygon(left);
        p.drawPolygon(right);
    } else if (kind == "Texture") {
        p.drawRoundedRect(QRectF(17, 22, 62, 52), 4, 4);
        p.drawEllipse(QPointF(61, 36), 5, 5);
        p.drawPolyline(QPolygonF{{20, 67}, {37, 45}, {49, 60}, {58, 50}, {76, 68}});
    } else if (kind == "Play") {
        p.setBrush(QColor("#83c799"));
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{{33, 22}, {75, 48}, {33, 74}});
    } else if (kind == "Back" || kind == "Forward" || kind == "Up") {
        p.translate(48, 48);
        if (kind == "Forward")
            p.rotate(180);
        if (kind == "Up")
            p.rotate(90);
        p.drawPolyline(QPolygonF{{8, -20}, {-12, 0}, {8, 20}});
    } else if (kind == "Search") {
        p.drawEllipse(QPointF(42, 41), 19, 19);
        p.drawLine(56, 56, 75, 75);
    } else if (kind == "Scene") {
        p.drawRoundedRect(QRectF(18, 20, 60, 56), 4, 4);
        p.drawLine(18, 34, 78, 34);
        p.drawLine(33, 20, 43, 34);
        p.drawLine(54, 20, 64, 34);
        p.drawPolygon(QPolygonF{{39, 43}, {59, 55}, {39, 67}});
    } else if (kind == "Save") {
        p.drawRoundedRect(QRectF(22, 18, 52, 60), 3, 3);
        p.drawRect(QRectF(32, 18, 28, 21));
        p.drawRect(QRectF(32, 54, 32, 24));
    } else {
        p.drawRoundedRect(QRectF(25, 17, 46, 63), 4, 4);
        p.drawLine(36, 39, 60, 39);
        p.drawLine(36, 51, 60, 51);
        p.drawLine(36, 63, 54, 63);
    }
    const QIcon icon(image);
    cache.insert(kind, icon);
    return icon;
}

void apply_editor_style(QApplication &app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#262626"));
    palette.setColor(QPalette::WindowText, QColor("#d5d5d5"));
    palette.setColor(QPalette::Base, QColor("#1b1b1b"));
    palette.setColor(QPalette::AlternateBase, QColor("#202020"));
    palette.setColor(QPalette::Text, QColor("#dddddd"));
    palette.setColor(QPalette::Button, QColor("#333333"));
    palette.setColor(QPalette::ButtonText, QColor("#dddddd"));
    palette.setColor(QPalette::Highlight, QColor("#297ac2"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor("#858585"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#737373"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#737373"));
    app.setPalette(palette);
    auto font = app.font();
    font.setPixelSize(12);
    app.setFont(font);
    app.setStyleSheet(R"(
        QMainWindow { background:#141414; }
        QMenuBar,QMenu { background:#252525; padding:3px; }
        QMenuBar::item:selected,QMenu::item:selected { background:#3b3b3b; }
        QToolBar { background:#222222; border:none; border-bottom:1px solid #111111; spacing:5px; padding:5px; }
        QDockWidget { titlebar-close-icon:none; font-weight:600; }
        QDockWidget::title { background:#303030; padding:7px 10px; border-bottom:1px solid #111111; }
        QDockWidget::close-button,QDockWidget::float-button { border:none; padding:3px; }
        QTreeView,QListView { background:#1b1b1b; border:none; outline:none; }
        QTreeView::item { padding:4px 2px; border-radius:2px; }
        QTreeView::item:hover { background:#303030; }
        QTreeView::item:selected { background:#254b6d; color:#ffffff; }
        QHeaderView::section { background:#292929; border:none; padding:5px; color:#a6a6a6; }
        QLineEdit,QDoubleSpinBox,QComboBox { background:#191919; border:1px solid #424242; border-radius:4px; padding:5px 7px; selection-background-color:#297ac2; }
        QLineEdit:focus,QDoubleSpinBox:focus,QComboBox:focus { border-color:#529ad5; }
        QPushButton { background:#333333; border:1px solid #454545; border-radius:4px; padding:5px 9px; }
        QPushButton:hover { background:#414141; border-color:#646464; }
        QPushButton:pressed { background:#244c70; }
        QPushButton:disabled { background:#282828; border-color:#333333; color:#737373; }
        QToolButton { border:none; border-radius:3px; padding:4px; }
        QToolButton:hover { background:#414141; }
        QToolButton:checked { background:#284d6f; }
        QScrollBar:vertical { background:#202020; width:10px; margin:0; }
        QScrollBar:horizontal { background:#202020; height:10px; margin:0; }
        QScrollBar::handle { background:#4c4c4c; border-radius:4px; min-width:22px; min-height:22px; margin:2px; }
        QScrollBar::handle:hover { background:#666666; }
        QScrollBar::add-line,QScrollBar::sub-line { width:0; height:0; }
        QScrollBar::add-page,QScrollBar::sub-page { background:none; }
        QSplitter::handle { background:#121212; width:3px; height:3px; }
        QStatusBar { background:#222222; color:#ababab; border-top:1px solid #141414; padding:2px; }
        QToolTip { background:#303030; color:#eeeeee; border:1px solid #555555; padding:5px; }
        QLabel[section="true"] { background:#292929; color:#a9a9a9; font-weight:600; padding:6px 8px; }
        QLabel#AssetPreview { background:#151515; border:1px solid #333333; border-radius:5px; }
    )");
}

bool editor_asset_inside(const QString &root, const QString &candidate) {
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(canonical);
    return !canonical.isEmpty() && !relative.startsWith("../") && relative != ".." &&
           !QDir::isAbsolutePath(relative);
}
