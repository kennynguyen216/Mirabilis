#include "qt_asset_delegate.h"
#include "qt_editor_style.h"
#include <QAbstractItemView>
#include <QDateTime>
#include <QFileSystemModel>
#include <QImageReader>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QRunnable>
#include <QSortFilterProxyModel>

QtAssetDelegate::QtAssetDelegate(QObject *parent) : QStyledItemDelegate(parent) {
    workers_.setMaxThreadCount(2);
}
QtAssetDelegate::~QtAssetDelegate() {
    workers_.clear();
    workers_.waitForDone();
}

QSize QtAssetDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    if (auto *view = qobject_cast<QListView *>(parent()))
        return view->gridSize();
    return {120, 124};
}

void QtAssetDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    auto source = index;
    const QAbstractItemModel *model = index.model();
    if (const auto *proxy = qobject_cast<const QSortFilterProxyModel *>(model)) {
        source = proxy->mapToSource(index);
        model = proxy->sourceModel();
    }
    const auto *files = qobject_cast<const QFileSystemModel *>(model);
    if (!files) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const auto info = files->fileInfo(source);
    const auto kind = asset_kind(info.absoluteFilePath());
    const auto key = info.absoluteFilePath() + ":" +
                     QString::number(info.lastModified().toMSecsSinceEpoch()) + ":" +
                     QString::number(info.size());
    const QColor accent = kind == "Texture" ? QColor("#b18acb")
                          : kind == "Mesh"  ? QColor("#6faed1")
                          : kind == "Scene" ? QColor("#7db895")
                                            : QColor("#b89a65");
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect card = option.rect.adjusted(4, 4, -4, -4);
    const bool selected = option.state & QStyle::State_Selected;
    painter->setPen(selected ? QPen(QColor("#589bcc"), 1) : QPen(QColor("#363636"), 1));
    painter->setBrush(selected ? QColor("#263f53")
                               : (option.state & QStyle::State_MouseOver ? QColor("#303030")
                                                                         : QColor("#242424")));
    painter->drawRoundedRect(card, 5, 5);
    const QRect preview(card.x() + 6, card.y() + 5, card.width() - 12, 64);
    const auto *thumb = thumbnails_.object(key);
    if (thumb && !thumb->isNull()) {
        const auto scaled = thumb->size().scaled(preview.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint(preview.center().x() - scaled.width() / 2,
                                  preview.center().y() - scaled.height() / 2),
                           scaled);
        painter->drawImage(target, *thumb);
    } else {
        editor_icon(kind).paint(painter, preview, Qt::AlignCenter);
        if (kind == "Texture" && editor_asset_inside(files->rootPath(), info.absoluteFilePath()) &&
            !thumbnails_.contains(key) && !pending_.contains(key) && pending_.size() < 24) {
            pending_.insert(key);
            const QPointer<QtAssetDelegate> self(const_cast<QtAssetDelegate *>(this));
            const auto filename = info.canonicalFilePath();
            workers_.start(QRunnable::create([self, key, filename] {
                QImageReader reader(filename);
                const auto size = reader.size();
                QImage image;
                if (size.isValid() && qint64(size.width()) * size.height() <= 64 * 1024 * 1024) {
                    reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
                    image = reader.read();
                }
                if (self)
                    QMetaObject::invokeMethod(
                        self,
                        [self, key, image = std::move(image)] {
                            if (!self)
                                return;
                            self->pending_.remove(key);
                            // Cache failures too, so a visible unsupported file doesn't decode
                            // every repaint.
                            self->thumbnails_.insert(key, new QImage(image));
                            if (auto *view = qobject_cast<QAbstractItemView *>(self->parent()))
                                view->viewport()->update();
                        },
                        Qt::QueuedConnection);
            }));
        }
    }
    painter->setPen(QColor("#dedede"));
    auto font = option.font;
    font.setPixelSize(11);
    painter->setFont(font);
    const auto name =
        QFontMetrics(font).elidedText(info.completeBaseName(), Qt::ElideMiddle, card.width() - 12);
    painter->drawText(QRect(card.x() + 6, card.y() + 74, card.width() - 12, 16),
                      Qt::AlignLeft | Qt::AlignVCenter, name);
    painter->setPen(accent);
    font.setPixelSize(10);
    painter->setFont(font);
    painter->drawText(QRect(card.x() + 6, card.y() + 92, card.width() - 12, 14),
                      Qt::AlignLeft | Qt::AlignVCenter, kind);
    painter->fillRect(QRect(card.x() + 2, card.bottom() - 2, card.width() - 3, 2), accent);
    painter->restore();
}
