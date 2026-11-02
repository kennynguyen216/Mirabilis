#pragma once
#include <QCache>
#include <QImage>
#include <QSet>
#include <QStyledItemDelegate>
#include <QThreadPool>

class QtAssetDelegate final : public QStyledItemDelegate {
  public:
    explicit QtAssetDelegate(QObject *parent);
    ~QtAssetDelegate() override;
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;

  private:
    mutable QCache<QString, QImage> thumbnails_{128};
    mutable QSet<QString> pending_;
    mutable QThreadPool workers_;
};
