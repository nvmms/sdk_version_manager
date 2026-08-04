#pragma once

#include <QString>

class QNetworkAccessManager;

QString configuredProxyUrl();
bool setConfiguredProxyUrl(const QString &url, QString *errorMessage = nullptr);
bool clearConfiguredProxy(QString *errorMessage = nullptr);
bool applyConfiguredProxy(QNetworkAccessManager &manager, QString *errorMessage = nullptr);

