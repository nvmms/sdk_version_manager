#include "networkproxyconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {
QString configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/settings/network.json");
}

bool parseProxy(const QString &text, QNetworkProxy *proxy, QString *errorMessage)
{
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || scheme != QStringLiteral("http")) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Only HTTP proxy URLs are supported.");
        return false;
    }
    if (!url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery()
        || url.hasFragment()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Proxy credentials, paths, queries, and fragments are not allowed.");
        return false;
    }
    const int port = url.port(8080);
    if (port <= 0 || port > 65535) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Proxy port must be between 1 and 65535.");
        return false;
    }
    *proxy = QNetworkProxy(QNetworkProxy::HttpProxy, url.host(),
                          static_cast<quint16>(port));
    return true;
}

bool writeConfig(const QJsonObject &config, QString *errorMessage)
{
    const QString path = configPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to create the settings directory.");
        return false;
    }
    QSaveFile file(path);
    const QByteArray data = QJsonDocument(config).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to write %1").arg(path);
        return false;
    }
    return true;
}
}

QString configuredProxyUrl()
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("url")).toString();
}

bool setConfiguredProxyUrl(const QString &url, QString *errorMessage)
{
    QNetworkProxy proxy;
    if (!parseProxy(url, &proxy, errorMessage))
        return false;
    Q_UNUSED(proxy)
    return writeConfig(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                   {QStringLiteral("url"), url}}, errorMessage);
}

bool clearConfiguredProxy(QString *errorMessage)
{
    return writeConfig(QJsonObject{{QStringLiteral("schemaVersion"), 1}}, errorMessage);
}

bool applyConfiguredProxy(QNetworkAccessManager &manager, QString *errorMessage)
{
    const QString url = configuredProxyUrl();
    if (url.isEmpty())
        return true;
    QNetworkProxy proxy;
    if (!parseProxy(url, &proxy, errorMessage))
        return false;
    manager.setProxy(proxy);
    return true;
}
