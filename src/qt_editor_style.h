#pragma once
#include <QIcon>
#include <QString>
class QApplication;
void apply_editor_style(QApplication &app);
QIcon editor_icon(const QString &kind);
QString asset_kind(const QString &path);

bool editor_asset_inside(const QString &root, const QString &candidate);
