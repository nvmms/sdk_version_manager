#include "providercontroller.h"
#include "eventbus.h"
#include "networkproxyconfig.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSharedPointer>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#endif

namespace {
constexpr auto nodeIndexUrl = "https://nodejs.org/dist/index.json";
constexpr auto nodeDistBase = "https://nodejs.org/dist/";
constexpr auto flutterIndexUrl =
    "https://storage.googleapis.com/flutter_infra_release/releases/releases_windows.json";
constexpr auto javaIndexBase = "https://api.adoptium.net/v3/assets/feature_releases/";
constexpr auto pythonIndexUrl = "https://www.python.org/api/v2/downloads/release_file/?os=1";
constexpr auto phpIndexUrl = "https://windows.php.net/downloads/releases/releases.json";
constexpr auto phpDistBase = "https://windows.php.net/downloads/releases/";
constexpr auto phpArchiveUrl = "https://windows.php.net/downloads/releases/archives/";
constexpr auto goIndexUrl = "https://go.dev/dl/?mode=json&include=all";
constexpr auto postgresqlIndexUrl =
    "https://www.postgresql.org/versions.json";
constexpr auto mavenIndexUrl = "https://maven.apache.org/docs/history.html";
constexpr auto mavenArchiveBase = "https://archive.apache.org/dist/maven/";
constexpr auto goDistBase = "https://go.dev/dl/";
}

ProviderController::ProviderController(QObject *parent)
    : QObject(parent)
{
    QString proxyError;
    if (!applyConfiguredProxy(m_network, &proxyError))
        setError(proxyError);
    loadDefaults();
    const QString cliExecutable = QCoreApplication::applicationDirPath() + QStringLiteral("/svm.exe");
    if (QFileInfo(cliExecutable).isFile())
        writeCommandShim(QStringLiteral("svm"), cliExecutable);
    ensureShimPath();
}

QVariantList ProviderController::versions() const { return m_versions; }
bool ProviderController::busy() const { return m_busy; }
double ProviderController::progress() const { return m_progress; }
QString ProviderController::status() const { return m_status; }
QString ProviderController::error() const { return m_error; }
QString ProviderController::activeProvider() const { return m_providerId; }
QString ProviderController::activeVersion() const { return m_version; }
bool ProviderController::removing() const { return m_removing; }
bool ProviderController::settingDefault() const { return m_settingDefault; }
QVariantMap ProviderController::defaultVersions() const { return m_defaultVersions; }

void ProviderController::setVerbose(bool verbose)
{
    m_verbose = verbose;
    if (!m_verbose)
        return;
    const QNetworkProxy proxy = m_network.proxy();
    if (proxy.type() == QNetworkProxy::DefaultProxy
        || proxy.type() == QNetworkProxy::NoProxy) {
        emit diagnostic(QStringLiteral("network proxy: disabled"));
    } else {
        emit diagnostic(QStringLiteral("network proxy: HTTP %1:%2")
                            .arg(proxy.hostName()).arg(proxy.port()));
    }
}

void ProviderController::startEventBus()
{
    if (m_eventBus)
        return;
    m_eventBus = new SvmEventBus(this);
    connect(m_eventBus, &SvmEventBus::eventReceived, this, [this](const QJsonObject &event) {
        const QString provider = event.value(QStringLiteral("provider")).toString();
        const QString version = event.value(QStringLiteral("version")).toString();
        const QString type = event.value(QStringLiteral("type")).toString();
        if (provider.isEmpty() || version.isEmpty())
            return;

        const auto armExternalWatchdog = [this] {
            const quint64 serial = ++m_externalEventSerial;
            QTimer::singleShot(3500, this, [this, serial] {
                if (m_busy && serial == m_externalEventSerial) {
                    setStatus(tr("CLI 下载已停止，可再次执行命令继续下载"));
                    setBusy(false);
                }
            });
        };

        if (m_providerId != provider) {
            m_providerId = provider;
            emit activeProviderChanged();
        }
        if (m_version != version) {
            m_version = version;
            emit activeVersionChanged();
        }

        if (type == QStringLiteral("download-start")) {
            setError({});
            setProgress(0.0);
            setStatus(tr("CLI 正在下载 %1 %2…").arg(provider, version));
            setBusy(true);
            armExternalWatchdog();
        } else if (type == QStringLiteral("download-progress")) {
            setProgress(event.value(QStringLiteral("progress")).toDouble());
            setBusy(true);
            armExternalWatchdog();
        } else if (type == QStringLiteral("heartbeat")) {
            setProgress(event.value(QStringLiteral("progress")).toDouble());
            setBusy(true);
            armExternalWatchdog();
        } else if (type == QStringLiteral("install-start")) {
            setProgress(1.0);
            setStatus(tr("CLI 正在安装 %1 %2…").arg(provider, version));
            setBusy(true);
            armExternalWatchdog();
        } else if (type == QStringLiteral("done")) {
            ++m_externalEventSerial;
            setProgress(1.0);
            setStatus(tr("CLI 已完成 %1 %2").arg(provider, version));
            setBusy(false);
            if (isDownloaded(provider, version))
                emit downloadFinished(provider, version, downloadDirectory(provider, version));
        } else if (type == QStringLiteral("error")) {
            ++m_externalEventSerial;
            setError(tr("CLI 下载或安装 %1 %2 失败").arg(provider, version));
            setStatus({});
            setBusy(false);
        }
    });
    m_eventBus->start();
}

void ProviderController::loadVersions(const QString &providerId, bool forceRefresh)
{
    if (providerId != QStringLiteral("node") && providerId != QStringLiteral("flutter")
        && providerId != QStringLiteral("java") && providerId != QStringLiteral("python")
        && providerId != QStringLiteral("php") && providerId != QStringLiteral("go")
        && providerId != QStringLiteral("postgresql")
        && providerId != QStringLiteral("maven")) {
        setError(tr("%1 Provider 尚未接入官方版本源").arg(providerId));
        return;
    }
    if (m_busy) {
        return;
    }
    if (m_providerId != providerId) {
        m_providerId = providerId;
        emit activeProviderChanged();
        m_versions.clear();
        emit versionsChanged();
    }
    if (!m_version.isEmpty()) {
        m_version.clear();
        emit activeVersionChanged();
    }
    if (!forceRefresh && !m_versions.isEmpty()) {
        return;
    }
    if (!forceRefresh) {
        QFile cache(cachePath(providerId));
        if (cache.open(QIODevice::ReadOnly)) {
            const QByteArray data = cache.readAll();
            const bool phpCacheHasArchives =
                providerId != QStringLiteral("php")
                || QJsonDocument::fromJson(data).object().contains(QStringLiteral("archiveHtml"));
            const bool applied = providerId == QStringLiteral("node")
                ? applyNodeIndex(data)
                : providerId == QStringLiteral("flutter") ? applyFlutterIndex(data)
                : providerId == QStringLiteral("java") ? applyJavaIndex(data)
                : providerId == QStringLiteral("python") ? applyPythonIndex(data)
                : providerId == QStringLiteral("php") ? applyPhpIndex(data)
                : providerId == QStringLiteral("go") ? applyGoIndex(data)
                : providerId == QStringLiteral("maven") ? applyMavenIndex(data)
                                                        : applyPostgresqlIndex(data);
            if (applied && phpCacheHasArchives) {
                if (m_verbose)
                    emit diagnostic(QStringLiteral("version index: local cache %1")
                                        .arg(cachePath(providerId)));
                setStatus(tr("已读取本地 %1 版本缓存").arg(
                    providerId == QStringLiteral("node") ? QStringLiteral("Node.js")
                    : providerId == QStringLiteral("flutter") ? QStringLiteral("Flutter")
                    : providerId == QStringLiteral("java") ? QStringLiteral("Java")
                    : providerId == QStringLiteral("python") ? QStringLiteral("Python")
                    : providerId == QStringLiteral("php") ? QStringLiteral("PHP")
                    : providerId == QStringLiteral("go") ? QStringLiteral("Go")
                    : providerId == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                                                            : QStringLiteral("PostgreSQL")));
                return;
            }
        }
    }

    setError({});
    const QString displayName = providerId == QStringLiteral("node")
        ? QStringLiteral("Node.js")
        : providerId == QStringLiteral("flutter") ? QStringLiteral("Flutter")
        : providerId == QStringLiteral("java") ? QStringLiteral("Java")
        : providerId == QStringLiteral("python") ? QStringLiteral("Python")
        : providerId == QStringLiteral("php") ? QStringLiteral("PHP")
        : providerId == QStringLiteral("go") ? QStringLiteral("Go")
        : providerId == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                                                : QStringLiteral("PostgreSQL");
    setStatus(tr("正在从 %1 官方源获取版本…").arg(displayName));
    setProgress(0.0);
    setBusy(true);
    if (providerId == QStringLiteral("java")) {
        fetchJavaIndexes();
        return;
    }
    if (providerId == QStringLiteral("php")) {
        fetchPhpIndexes();
        return;
    }
    const QUrl indexUrl(providerId == QStringLiteral("node")
                            ? QString::fromLatin1(nodeIndexUrl)
                        : providerId == QStringLiteral("flutter")
                            ? QString::fromLatin1(flutterIndexUrl)
                        : providerId == QStringLiteral("python")
                            ? QString::fromLatin1(pythonIndexUrl)
                        : providerId == QStringLiteral("php")
                            ? QString::fromLatin1(phpIndexUrl)
                        : providerId == QStringLiteral("maven")
                            ? QString::fromLatin1(mavenIndexUrl)
                        : providerId == QStringLiteral("postgresql")
                            ? QString::fromLatin1(postgresqlIndexUrl)
                            : QString::fromLatin1(goIndexUrl));
    if (m_verbose)
        emit diagnostic(QStringLiteral("GET %1").arg(indexUrl.toString()));
    m_reply = m_network.get(QNetworkRequest(indexUrl));
    connect(m_reply, &QNetworkReply::finished, this, [this, providerId, displayName] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (m_verbose) {
            emit diagnostic(QStringLiteral("HTTP %1 %2%3")
                                .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                                .arg(reply->url().toString(),
                                     reply->error() == QNetworkReply::NoError
                                         ? QString() : QStringLiteral(" error=") + reply->errorString()));
        }
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            fail(tr("获取 %1 版本失败：%2").arg(displayName, message));
            return;
        }

        const QByteArray data = reply->readAll();
        reply->deleteLater();
        const bool applied = providerId == QStringLiteral("node")
            ? applyNodeIndex(data)
            : providerId == QStringLiteral("flutter") ? applyFlutterIndex(data)
            : providerId == QStringLiteral("java") ? applyJavaIndex(data)
            : providerId == QStringLiteral("python") ? applyPythonIndex(data)
            : providerId == QStringLiteral("php") ? applyPhpIndex(data)
            : providerId == QStringLiteral("go") ? applyGoIndex(data)
            : providerId == QStringLiteral("maven") ? applyMavenIndex(data)
                                                    : applyPostgresqlIndex(data);
        if (!applied) {
            fail(tr("%1 版本索引格式无效").arg(displayName));
            return;
        }

        QSaveFile cache(cachePath(providerId));
        const QFileInfo cacheInfo(cachePath(providerId));
        QDir().mkpath(cacheInfo.absolutePath());
        if (cache.open(QIODevice::WriteOnly)) {
            cache.write(data);
            cache.commit();
        }

        setStatus(tr("已从 %1 官方源获取 %2 个版本").arg(displayName).arg(m_versions.size()));
        setBusy(false);
    });
}

void ProviderController::fetchJavaIndexes()
{
    const QList<int> featureVersions{8, 11, 17, 21, 25};
    auto combined = QSharedPointer<QJsonArray>::create();
    auto remaining = QSharedPointer<int>::create(featureVersions.size());
    auto errors = QSharedPointer<QStringList>::create();
    m_javaReplies.clear();

    for (const int featureVersion : featureVersions) {
        const QUrl url(QString::fromLatin1(javaIndexBase) + QString::number(featureVersion)
                       + QStringLiteral("/ga?architecture=x64&heap_size=normal&image_type=jdk"
                                        "&jvm_impl=hotspot&os=windows&page=0&page_size=20"
                                        "&project=jdk&sort_method=DATE&sort_order=DESC&vendor=eclipse"));
        if (m_verbose)
            emit diagnostic(QStringLiteral("GET %1").arg(url.toString()));
        QNetworkReply *reply = m_network.get(QNetworkRequest(url));
        m_javaReplies.append(reply);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, combined, remaining, errors, featureVersion] {
            if (m_verbose) {
                emit diagnostic(QStringLiteral("HTTP %1 %2%3")
                                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                                    .arg(reply->url().toString(),
                                         reply->error() == QNetworkReply::NoError
                                             ? QString() : QStringLiteral(" error=") + reply->errorString()));
            }
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
                if (document.isArray()) {
                    for (const QJsonValue &value : document.array())
                        combined->append(value);
                } else {
                    errors->append(tr("Java %1 返回了无效索引").arg(featureVersion));
                }
            } else {
                errors->append(tr("Java %1：%2").arg(featureVersion).arg(reply->errorString()));
            }
            m_javaReplies.removeAll(reply);
            reply->deleteLater();
            --*remaining;
            if (*remaining != 0)
                return;

            const QByteArray data = QJsonDocument(*combined).toJson(QJsonDocument::Compact);
            if (!applyJavaIndex(data)) {
                fail(errors->isEmpty() ? tr("Java 版本索引格式无效")
                                       : tr("获取 Java 版本失败：%1").arg(errors->join(u';')));
                return;
            }
            QSaveFile cache(cachePath(QStringLiteral("java")));
            QDir().mkpath(QFileInfo(cachePath(QStringLiteral("java"))).absolutePath());
            if (cache.open(QIODevice::WriteOnly)) {
                cache.write(data);
                cache.commit();
            }
            setStatus(errors->isEmpty()
                          ? tr("已从 Eclipse Adoptium 获取 %1 个 Java 版本").arg(m_versions.size())
                          : tr("已获取 %1 个 Java 版本；部分主版本暂不可用").arg(m_versions.size()));
            setBusy(false);
        });
    }
}

void ProviderController::fetchPhpIndexes()
{
    auto current = QSharedPointer<QJsonObject>::create();
    auto archiveHtml = QSharedPointer<QByteArray>::create();
    auto remaining = QSharedPointer<int>::create(2);
    auto errors = QSharedPointer<QStringList>::create();
    const QList<QUrl> urls{QUrl(QString::fromLatin1(phpIndexUrl)),
                           QUrl(QString::fromLatin1(phpArchiveUrl))};

    for (int index = 0; index < urls.size(); ++index) {
        QNetworkReply *reply = m_network.get(QNetworkRequest(urls[index]));
        m_javaReplies.append(reply);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, index, current, archiveHtml, remaining, errors] {
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray body = reply->readAll();
                if (index == 0) {
                    const QJsonDocument document = QJsonDocument::fromJson(body);
                    if (document.isObject())
                        *current = document.object();
                    else
                        errors->append(tr("PHP 当前版本索引格式无效"));
                } else {
                    *archiveHtml = body;
                }
            } else {
                errors->append(reply->errorString());
            }
            m_javaReplies.removeAll(reply);
            reply->deleteLater();
            if (--*remaining != 0)
                return;

            QJsonObject combined;
            combined.insert(QStringLiteral("schemaVersion"), 1);
            combined.insert(QStringLiteral("current"), *current);
            combined.insert(QStringLiteral("archiveHtml"), QString::fromUtf8(*archiveHtml));
            const QByteArray data =
                QJsonDocument(combined).toJson(QJsonDocument::Compact);
            if (!applyPhpIndex(data)) {
                fail(tr("获取 PHP 版本失败：%1").arg(errors->join(u';')));
                return;
            }
            QSaveFile cache(cachePath(QStringLiteral("php")));
            QDir().mkpath(QFileInfo(cachePath(QStringLiteral("php"))).absolutePath());
            if (cache.open(QIODevice::WriteOnly)) {
                cache.write(data);
                cache.commit();
            }
            setStatus(errors->isEmpty()
                          ? tr("已获取 %1 个 PHP 当前及历史版本").arg(m_versions.size())
                          : tr("已获取 %1 个 PHP 版本；部分索引暂不可用").arg(m_versions.size()));
            setBusy(false);
        });
    }
}

bool ProviderController::applyNodeIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        return false;
    }

    QVariantList versions;
    const QJsonArray releases = document.array();
    for (const QJsonValue &value : releases) {
        const QJsonObject release = value.toObject();
        const QJsonArray files = release.value(QStringLiteral("files")).toArray();
        bool supportsWindowsX64Zip = false;
        for (const QJsonValue &file : files) {
            if (file.toString() == QStringLiteral("win-x64-zip")) {
                supportsWindowsX64Zip = true;
                break;
            }
        }
        if (!supportsWindowsX64Zip) {
            continue;
        }

        const QString rawVersion = release.value(QStringLiteral("version")).toString();
        const QString version = rawVersion.startsWith(u'v') ? rawVersion.mid(1) : rawVersion;
        const QJsonValue ltsValue = release.value(QStringLiteral("lts"));
        const QString channel = ltsValue.isString()
            ? tr("LTS · %1").arg(ltsValue.toString())
            : QStringLiteral("current");

        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"), channel);
        item.insert(QStringLiteral("released"), release.value(QStringLiteral("date")).toString());
        item.insert(QStringLiteral("size"), QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), ltsValue.isString());
        versions.append(item);
    }
    if (versions.isEmpty()) {
        return false;
    }

    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyFlutterIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    const QJsonObject root = document.object();
    const QString baseUrl = root.value(QStringLiteral("base_url")).toString();
    const QJsonObject current = root.value(QStringLiteral("current_release")).toObject();
    QVariantList versions;
    for (const QJsonValue &value : root.value(QStringLiteral("releases")).toArray()) {
        const QJsonObject release = value.toObject();
        const QString arch = release.value(QStringLiteral("dart_sdk_arch")).toString();
        const QString archive = release.value(QStringLiteral("archive")).toString();
        const QString sha256 = release.value(QStringLiteral("sha256")).toString();
        const QString channel = release.value(QStringLiteral("channel")).toString();
        if ((!arch.isEmpty() && arch != QStringLiteral("x64"))
            || archive.isEmpty() || sha256.size() != 64) {
            continue;
        }

        QVariantMap item;
        item.insert(QStringLiteral("version"), release.value(QStringLiteral("version")).toString());
        item.insert(QStringLiteral("channel"), channel);
        item.insert(QStringLiteral("released"),
                    release.value(QStringLiteral("release_date")).toString().left(10));
        item.insert(QStringLiteral("size"), QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"),
                    current.value(channel).toString()
                        == release.value(QStringLiteral("hash")).toString());
        item.insert(QStringLiteral("downloadUrl"), baseUrl + u'/' + archive);
        item.insert(QStringLiteral("sha256"), sha256.toLower());
        versions.append(item);
    }
    if (versions.isEmpty()) {
        return false;
    }
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyJavaIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return false;

    QVariantList versions;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject release = value.toObject();
        const QJsonArray binaries = release.value(QStringLiteral("binaries")).toArray();
        if (binaries.isEmpty())
            continue;
        const QJsonObject binary = binaries.first().toObject();
        const QJsonObject package = binary.value(QStringLiteral("package")).toObject();
        const QJsonObject versionData = release.value(QStringLiteral("version_data")).toObject();
        const QString version = QStringLiteral("%1.%2.%3")
            .arg(versionData.value(QStringLiteral("major")).toInt())
            .arg(versionData.value(QStringLiteral("minor")).toInt())
            .arg(versionData.value(QStringLiteral("security")).toInt());
        const QString link = package.value(QStringLiteral("link")).toString();
        const QString checksum = package.value(QStringLiteral("checksum")).toString().toLower();
        if (version.startsWith(QStringLiteral("0.")) || !QUrl(link).isValid()
            || checksum.size() != 64)
            continue;

        const int major = versionData.value(QStringLiteral("major")).toInt();
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"),
                    QList<int>{8, 11, 17, 21, 25}.contains(major)
                        ? QStringLiteral("LTS") : QStringLiteral("stable"));
        item.insert(QStringLiteral("released"),
                    release.value(QStringLiteral("release_date")).toString().left(10));
        const qint64 size = package.value(QStringLiteral("size")).toInteger();
        item.insert(QStringLiteral("size"),
                    size > 0 ? QStringLiteral("%1 MB").arg(size / 1024 / 1024)
                             : QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), major == 25);
        item.insert(QStringLiteral("downloadUrl"), link);
        item.insert(QStringLiteral("sha256"), checksum);
        versions.append(item);
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyPythonIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return false;

    static const QRegularExpression installerPattern(
        QStringLiteral(R"(/python/(\d+\.\d+\.\d+)/python-\1-amd64\.exe$)"));
    QVariantList versions;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject file = value.toObject();
        if (file.value(QStringLiteral("name")).toString()
            != QStringLiteral("Windows installer (64-bit)")) {
            continue;
        }
        const QString url = file.value(QStringLiteral("url")).toString();
        const QRegularExpressionMatch match = installerPattern.match(url);
        const QString checksum = file.value(QStringLiteral("sha256_sum")).toString().toLower();
        if (!match.hasMatch())
            continue;

        const QString version = match.captured(1);
        const QStringList versionParts = version.split(u'.');
        if (versionParts.value(0).toInt() != 3 || versionParts.value(1).toInt() < 10)
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"), QStringLiteral("stable"));
        item.insert(QStringLiteral("released"), QStringLiteral("—"));
        const qint64 size = file.value(QStringLiteral("filesize")).toInteger();
        item.insert(QStringLiteral("size"),
                    size > 0 ? QStringLiteral("%1 MB").arg(size / 1024 / 1024)
                             : QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), version.startsWith(QStringLiteral("3.14.")));
        item.insert(QStringLiteral("downloadUrl"), url);
        item.insert(QStringLiteral("sha256"), checksum);
        item.insert(QStringLiteral("verification"),
                    checksum.size() == 64 ? QStringLiteral("sha256")
                                          : QStringLiteral("authenticode"));
        versions.append(item);
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyPhpIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;

    QVariantList versions;
    const QJsonObject documentRoot = document.object();
    const QJsonObject root = documentRoot.contains(QStringLiteral("current"))
        ? documentRoot.value(QStringLiteral("current")).toObject()
        : documentRoot;
    QSet<QString> knownVersions;
    int newestMinor = 0;
    for (auto releaseIt = root.constBegin(); releaseIt != root.constEnd(); ++releaseIt) {
        const QStringList parts =
            releaseIt.value().toObject().value(QStringLiteral("version")).toString().split(u'.');
        if (parts.size() >= 2 && parts[0].toInt() == 8)
            newestMinor = qMax(newestMinor, parts[1].toInt());
    }
    for (auto releaseIt = root.constBegin(); releaseIt != root.constEnd(); ++releaseIt) {
        const QJsonObject release = releaseIt.value().toObject();
        const QString version = release.value(QStringLiteral("version")).toString();
        const QStringList parts = version.split(u'.');
        if (parts.size() < 3 || parts[0].toInt() != 8 || parts[1].toInt() < 1)
            continue;

        const QStringList buildTypes{QStringLiteral("nts"), QStringLiteral("ts")};
        for (const QString &buildType : buildTypes) {
            QJsonObject build;
            QString buildName;
            for (auto buildIt = release.constBegin(); buildIt != release.constEnd(); ++buildIt) {
                const bool matchingType = buildType == QStringLiteral("nts")
                    ? buildIt.key().startsWith(QStringLiteral("nts-"))
                    : buildIt.key().startsWith(QStringLiteral("ts-"));
                if (matchingType && buildIt.key().endsWith(QStringLiteral("-x64"))) {
                    build = buildIt.value().toObject();
                    buildName = buildIt.key();
                    break;
                }
            }
            const QJsonObject archive = build.value(QStringLiteral("zip")).toObject();
            const QString fileName = archive.value(QStringLiteral("path")).toString();
            const QString checksum = archive.value(QStringLiteral("sha256")).toString().toLower();
            if (buildName.isEmpty() || fileName.isEmpty() || checksum.size() != 64)
                continue;

            QVariantMap item;
            item.insert(QStringLiteral("version"), version + u'-' + buildType);
            item.insert(QStringLiteral("channel"),
                        parts[1].toInt() >= 4 ? QStringLiteral("stable")
                                              : QStringLiteral("security"));
            item.insert(QStringLiteral("buildType"), buildType);
            item.insert(QStringLiteral("released"),
                        build.value(QStringLiteral("mtime")).toString().left(10));
            item.insert(QStringLiteral("size"), archive.value(QStringLiteral("size")).toString());
            item.insert(QStringLiteral("recommended"),
                        parts[1].toInt() == newestMinor
                            && buildType == QStringLiteral("nts"));
            item.insert(QStringLiteral("downloadUrl"),
                        QString::fromLatin1(phpDistBase) + fileName);
            item.insert(QStringLiteral("sha256"), checksum);
            item.insert(QStringLiteral("build"), buildName);
            versions.append(item);
            knownVersions.insert(item.value(QStringLiteral("version")).toString());
        }
    }

    const QString archiveHtml =
        documentRoot.value(QStringLiteral("archiveHtml")).toString();
    static const QRegularExpression archivePattern(
        QStringLiteral(
            R"(php-(\d+\.\d+\.\d+)(-nts)?-Win32-[A-Za-z0-9]+-x64\.zip)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator matches = archivePattern.globalMatch(archiveHtml);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString baseVersion = match.captured(1);
        const QStringList parts = baseVersion.split(u'.');
        if (parts.size() < 3 || parts[0].toInt() < 5)
            continue;
        const QString buildType =
            match.captured(2).isEmpty() ? QStringLiteral("ts") : QStringLiteral("nts");
        const QString version = baseVersion + u'-' + buildType;
        if (knownVersions.contains(version))
            continue;
        const QString fileName = match.captured(0);
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"), QStringLiteral("legacy"));
        item.insert(QStringLiteral("buildType"), buildType);
        item.insert(QStringLiteral("released"), QStringLiteral("—"));
        item.insert(QStringLiteral("size"), QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), false);
        item.insert(QStringLiteral("downloadUrl"),
                    QString::fromLatin1(phpArchiveUrl) + fileName);
        item.insert(QStringLiteral("sha256"), QString());
        item.insert(QStringLiteral("verification"), QStringLiteral("unverified"));
        item.insert(QStringLiteral("unverified"), true);
        versions.append(item);
        knownVersions.insert(version);
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyGoIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return false;

    static const QRegularExpression versionPattern(
        QStringLiteral(R"(^go(\d+)\.(\d+)(?:\.(\d+))?(.*)$)"));
    QVariantList versions;
    bool recommendedAssigned = false;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject release = value.toObject();
        const QRegularExpressionMatch versionMatch =
            versionPattern.match(release.value(QStringLiteral("version")).toString());
        if (!versionMatch.hasMatch())
            continue;

        QJsonObject archive;
        for (const QJsonValue &fileValue : release.value(QStringLiteral("files")).toArray()) {
            const QJsonObject file = fileValue.toObject();
            if (file.value(QStringLiteral("os")).toString() == QStringLiteral("windows")
                && file.value(QStringLiteral("arch")).toString() == QStringLiteral("amd64")
                && file.value(QStringLiteral("kind")).toString() == QStringLiteral("archive")
                && file.value(QStringLiteral("filename")).toString().endsWith(
                    QStringLiteral(".zip"))) {
                archive = file;
                break;
            }
        }
        const QString fileName = archive.value(QStringLiteral("filename")).toString();
        const QString checksum = archive.value(QStringLiteral("sha256")).toString().toLower();
        if (fileName.isEmpty() || checksum.size() != 64)
            continue;

        const QString suffix = versionMatch.captured(4);
        QString version = QStringLiteral("%1.%2.%3")
                              .arg(versionMatch.captured(1), versionMatch.captured(2),
                                   versionMatch.captured(3).isEmpty()
                                       ? QStringLiteral("0") : versionMatch.captured(3));
        if (!suffix.isEmpty())
            version += u'-' + suffix;
        const bool stable = release.value(QStringLiteral("stable")).toBool();
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"),
                    stable ? QStringLiteral("stable") : QStringLiteral("beta"));
        item.insert(QStringLiteral("released"), QStringLiteral("—"));
        const qint64 size = archive.value(QStringLiteral("size")).toInteger();
        item.insert(QStringLiteral("size"),
                    size > 0 ? QStringLiteral("%1 MB").arg(size / 1024 / 1024)
                             : QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), stable && !recommendedAssigned);
        item.insert(QStringLiteral("downloadUrl"),
                    QString::fromLatin1(goDistBase) + fileName);
        item.insert(QStringLiteral("sha256"), checksum);
        versions.append(item);
        if (stable)
            recommendedAssigned = true;
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyPostgresqlIndex(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return false;
    QVariantList versions;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject release = value.toObject();
        if (!release.value(QStringLiteral("supported")).toBool())
            continue;
        const QString major = release.value(QStringLiteral("major")).toString();
        const QString minor = release.value(QStringLiteral("latestMinor")).toString();
        if (major.toInt() < 13 || minor.isEmpty())
            continue;
        const QString version = major + u'.' + minor;
        const QString fileName =
            QStringLiteral("postgresql-%1-1-windows-x64-binaries.zip").arg(version);
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"), QStringLiteral("stable"));
        item.insert(QStringLiteral("released"),
                    release.value(QStringLiteral("relDate")).toString());
        item.insert(QStringLiteral("size"), QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"),
                    release.value(QStringLiteral("current")).toBool());
        item.insert(QStringLiteral("downloadUrl"),
                    QStringLiteral("https://get.enterprisedb.com/postgresql/%1").arg(fileName));
        item.insert(QStringLiteral("verification"), QStringLiteral("unverified"));
        item.insert(QStringLiteral("unverified"), true);
        versions.append(item);
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

bool ProviderController::applyMavenIndex(const QByteArray &data)
{
    const QString html = QString::fromUtf8(data);
    static const QRegularExpression versionPattern(
        QStringLiteral(R"(>(3\.(?:8|9|10)\.\d+|4\.0\.0-(?:alpha|beta|rc)-\d+)<)"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> seen;
    QVariantList versions;
    bool recommendedAssigned = false;
    QRegularExpressionMatchIterator matches = versionPattern.globalMatch(html);
    while (matches.hasNext()) {
        const QString version = matches.next().captured(1).toLower();
        if (seen.contains(version))
            continue;
        seen.insert(version);
        const bool preview = version.startsWith(QStringLiteral("4."));
        const QString series = preview ? QStringLiteral("maven-4") : QStringLiteral("maven-3");
        const QString fileName = QStringLiteral("apache-maven-%1-bin.zip").arg(version);
        QVariantMap item;
        item.insert(QStringLiteral("version"), version);
        item.insert(QStringLiteral("channel"), preview ? QStringLiteral("preview")
                                                        : QStringLiteral("stable"));
        item.insert(QStringLiteral("released"), QStringLiteral("—"));
        item.insert(QStringLiteral("size"), QStringLiteral("—"));
        item.insert(QStringLiteral("recommended"), !preview && !recommendedAssigned);
        item.insert(QStringLiteral("minimumJava"), preview ? 17 : 8);
        item.insert(QStringLiteral("downloadUrl"),
                    QString::fromLatin1(mavenArchiveBase) + series + u'/' + version
                        + QStringLiteral("/binaries/") + fileName);
        item.insert(QStringLiteral("verification"), QStringLiteral("remote-sha512"));
        versions.append(item);
        if (!preview)
            recommendedAssigned = true;
    }
    if (versions.isEmpty())
        return false;
    m_versions = versions;
    emit versionsChanged();
    return true;
}

QString ProviderController::cachePath(const QString &providerId) const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/cache/") + providerId + QStringLiteral("/versions.json");
}

QString ProviderController::downloadDirectory(const QString &providerId, const QString &version) const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/downloads/") + providerId + u'/' + version;
}

QString ProviderController::downloadManifestPath(const QString &providerId, const QString &version) const
{
    return downloadDirectory(providerId, version) + QStringLiteral("/download.json");
}

void ProviderController::download(const QString &providerId, const QString &version,
                                  bool allowUnverified)
{
    if (providerId != QStringLiteral("node") && providerId != QStringLiteral("flutter")
        && providerId != QStringLiteral("java") && providerId != QStringLiteral("python")
        && providerId != QStringLiteral("php") && providerId != QStringLiteral("go")
        && providerId != QStringLiteral("postgresql")
        && providerId != QStringLiteral("maven")) {
        setError(tr("Provider %1 尚未接入真实下载").arg(providerId));
        return;
    }
    static const QRegularExpression safeVersion(
        QStringLiteral(R"(^\d+\.\d+(?:\.\d+)?(?:-[0-9A-Za-z.-]+)?$)"));
    if (!safeVersion.match(version).hasMatch()) {
        setError(tr("%1 版本号无效").arg(providerId));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    const QString directory = downloadDirectory(providerId, version);
    if (!QDir().mkpath(directory)) {
        setError(tr("无法创建下载目录：%1").arg(directory));
        return;
    }

    if (m_providerId != providerId) {
        m_providerId = providerId;
        emit activeProviderChanged();
    }
    if (m_version != version) {
        m_version = version;
        emit activeVersionChanged();
    }
    QUrl url;
    QString fileName;
    m_expectedHash.clear();
    m_verificationMode = QStringLiteral("sha256");
    if (providerId == QStringLiteral("node")) {
        fileName = nodeFileName(version);
        url = QUrl(QString::fromLatin1(nodeDistBase) + u'v' + version + u'/' + fileName);
    } else {
        QString verification;
        for (const QVariant &entry : std::as_const(m_versions)) {
            const QVariantMap release = entry.toMap();
            if (release.value(QStringLiteral("version")).toString() == version) {
                url = QUrl(release.value(QStringLiteral("downloadUrl")).toString());
                m_expectedHash = release.value(QStringLiteral("sha256")).toByteArray();
                verification = release.value(QStringLiteral("verification")).toString();
                break;
            }
        }
        fileName = QFileInfo(url.path()).fileName();
        const bool validVerification = m_expectedHash.size() == 64
            || (providerId == QStringLiteral("python")
                && verification == QStringLiteral("authenticode"))
            || (providerId == QStringLiteral("maven")
                && verification == QStringLiteral("remote-sha512"))
            || ((providerId == QStringLiteral("php")
                 || providerId == QStringLiteral("postgresql"))
                && verification == QStringLiteral("unverified") && allowUnverified);
        if ((providerId == QStringLiteral("php")
             || providerId == QStringLiteral("postgresql"))
            && verification == QStringLiteral("unverified") && !allowUnverified) {
            setError(tr("%1 %2 没有可自动校验的摘要；确认风险后才能安装")
                         .arg(providerId == QStringLiteral("php") ? QStringLiteral("PHP")
                                                                  : QStringLiteral("PostgreSQL"),
                              version));
            return;
        }
        if (!verification.isEmpty())
            m_verificationMode = verification;
        if (!url.isValid() || fileName.isEmpty() || !validVerification) {
            setError(tr("找不到 %1 %2 的官方安装包信息")
                         .arg(providerId == QStringLiteral("java") ? QStringLiteral("Java")
                              : providerId == QStringLiteral("python") ? QStringLiteral("Python")
                              : providerId == QStringLiteral("php") ? QStringLiteral("PHP")
                              : providerId == QStringLiteral("go") ? QStringLiteral("Go")
                              : providerId == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                              : providerId == QStringLiteral("postgresql")
                                  ? QStringLiteral("PostgreSQL")
                                  : QStringLiteral("Flutter"),
                              version));
            return;
        }
    }

    m_downloadPath = directory + u'/' + fileName;
    m_downloadFile.setFileName(m_downloadPath + QStringLiteral(".part"));
    m_resumeOffset = QFileInfo(m_downloadFile.fileName()).size();
    const QIODevice::OpenMode mode = QIODevice::WriteOnly
        | (m_resumeOffset > 0 ? QIODevice::Append : QIODevice::Truncate);
    if (!m_downloadFile.open(mode)) {
        setError(tr("无法写入下载文件：%1").arg(m_downloadFile.errorString()));
        return;
    }

    setError({});
    setProgress(0.0);
    setStatus(tr("正在下载 %1 %2…")
                  .arg(providerId == QStringLiteral("node") ? QStringLiteral("Node.js")
                       : providerId == QStringLiteral("flutter") ? QStringLiteral("Flutter")
                       : providerId == QStringLiteral("java") ? QStringLiteral("Java")
                       : providerId == QStringLiteral("python") ? QStringLiteral("Python")
                       : providerId == QStringLiteral("php") ? QStringLiteral("PHP")
                       : providerId == QStringLiteral("go") ? QStringLiteral("Go")
                       : providerId == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                                                            : QStringLiteral("PostgreSQL"),
                       version));
    setBusy(true);

    QNetworkRequest request(url);
    if (m_resumeOffset > 0) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_resumeOffset) + '-');
    }
    if (m_verbose) {
        emit diagnostic(QStringLiteral("GET %1%2")
                            .arg(url.toString(), m_resumeOffset > 0
                                ? QStringLiteral(" range=bytes=%1-").arg(m_resumeOffset)
                                : QString()));
    }
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (m_verbose && m_reply) {
            emit diagnostic(QStringLiteral("HTTP %1 content-length=%2")
                                .arg(m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                                .arg(m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()));
        }
        if (!m_reply || m_resumeOffset <= 0)
            return;
        const int status =
            m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200) {
            // The server ignored Range. Restart this response from byte zero
            // instead of appending a complete file to the partial file.
            m_downloadFile.resize(0);
            m_downloadFile.seek(0);
            m_resumeOffset = 0;
        }
    });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_reply && m_reply->isOpen() && m_reply->isReadable()) {
            m_downloadFile.write(m_reply->readAll());
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            setProgress(static_cast<double>(received + m_resumeOffset)
                        / static_cast<double>(total + m_resumeOffset));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (reply->isOpen() && reply->isReadable())
            m_downloadFile.write(reply->readAll());
        m_downloadFile.close();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 416 && m_resumeOffset > 0) {
            // A complete .part file can produce "range not satisfiable".
            // Verify it normally instead of downloading it again.
            reply->deleteLater();
            if (m_providerId == QStringLiteral("node"))
                fetchNodeChecksum();
            else if (m_providerId == QStringLiteral("maven"))
                fetchMavenChecksum();
            else if (m_verificationMode == QStringLiteral("unverified"))
                recordUnverifiedDownload();
            else
                verifyDownloadedFile(m_expectedHash);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            fail(tr("下载失败：%1").arg(message));
            return;
        }
        reply->deleteLater();
        if (m_providerId == QStringLiteral("node"))
            fetchNodeChecksum();
        else if (m_providerId == QStringLiteral("maven"))
            fetchMavenChecksum();
        else if (m_providerId == QStringLiteral("python") && m_expectedHash.isEmpty())
            verifyPythonDownload();
        else if (m_verificationMode == QStringLiteral("unverified"))
            recordUnverifiedDownload();
        else
            verifyDownloadedFile(m_expectedHash);
    });
}

bool ProviderController::isDownloaded(const QString &providerId, const QString &version) const
{
    static const QRegularExpression safeId(QStringLiteral(R"(^[a-z0-9][a-z0-9-]*$)"));
    static const QRegularExpression safeVersion(QStringLiteral(R"(^[0-9A-Za-z][0-9A-Za-z.-]*$)"));
    if (!safeId.match(providerId).hasMatch() || !safeVersion.match(version).hasMatch()) {
        return false;
    }

    QFile manifest(downloadManifestPath(providerId, version));
    if (manifest.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(manifest.readAll()).object();
        const QString fileName = object.value(QStringLiteral("file")).toString();
        if (!fileName.isEmpty()
            && QFileInfo(downloadDirectory(providerId, version) + u'/' + fileName).isFile()) {
            return true;
        }
    }

    // 兼容加入持久记录之前已经完成并校验过的 Node.js 下载。
    return providerId == QStringLiteral("node")
        && QFileInfo(downloadDirectory(providerId, version) + u'/' + nodeFileName(version)).isFile();
}

void ProviderController::setDefaultVersion(const QString &providerId, const QString &version)
{
    if (m_busy || m_settingDefault) {
        setError(tr("已有任务正在执行"));
        return;
    }
    if (!isDownloaded(providerId, version)) {
        setError(tr("必须先下载 %1 %2，才能设为默认版本").arg(providerId, version));
        return;
    }
    if (m_defaultVersions.value(providerId).toString() == version)
        return;

    if (m_providerId != providerId) {
        m_providerId = providerId;
        emit activeProviderChanged();
    }
    if (m_version != version) {
        m_version = version;
        emit activeVersionChanged();
    }
    setError({});
    setStatus(tr("正在设置 %1 %2 为默认版本…").arg(providerId, version));
    m_settingDefault = true;
    emit settingDefaultChanged();

    // Give QML one event-loop turn to repaint the disabled, busy button before
    // activation performs synchronous filesystem and environment updates.
    QTimer::singleShot(0, this, [this, providerId, version] {
        installDownloaded(providerId, version, true);
        if (!m_busy && m_settingDefault) {
            m_settingDefault = false;
            emit settingDefaultChanged();
        }
    });
}

void ProviderController::clearDefaultVersion(const QString &providerId)
{
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }
    if (!m_defaultVersions.contains(providerId))
        return;

    setError({});
    if (!deactivateProvider(providerId)) {
        setError(tr("无法完整移除 %1 的系统 PATH 指向").arg(providerId));
        return;
    }
    setStatus(tr("%1 的默认版本已取消").arg(providerId));
}

void ProviderController::installDownloaded(const QString &providerId, const QString &version,
                                           bool makeDefault)
{
    if (!isDownloaded(providerId, version)) {
        setError(tr("必须先下载 %1 %2，才能设为默认版本").arg(providerId, version));
        return;
    }
    if (makeDefault && m_defaultVersions.value(providerId).toString() == version) {
        return;
    }
    m_makeDefaultAfterInstall = makeDefault;
    if (providerId == QStringLiteral("flutter")) {
        installAndActivateFlutter(version);
        return;
    }
    if (providerId == QStringLiteral("java")) {
        installAndActivateJava(version);
        return;
    }
    if (providerId == QStringLiteral("python")) {
        installAndActivatePython(version);
        return;
    }
    if (providerId == QStringLiteral("php")) {
        installAndActivatePhp(version);
        return;
    }
    if (providerId == QStringLiteral("go")) {
        installAndActivateGo(version);
        return;
    }
    if (providerId == QStringLiteral("postgresql")) {
        installPostgresql(version);
        return;
    }
    if (providerId == QStringLiteral("maven")) {
        installMaven(version);
        return;
    }
    if (providerId != QStringLiteral("node")) {
        setError(tr("%1 Provider 尚未实现系统 PATH 激活").arg(providerId));
        return;
    }
    installAndActivateNode(version);
}

void ProviderController::removeDownloaded(const QString &providerId, const QString &version)
{
    static const QRegularExpression safeId(QStringLiteral(R"(^[a-z0-9][a-z0-9-]*$)"));
    static const QRegularExpression safeVersion(QStringLiteral(R"(^[0-9A-Za-z][0-9A-Za-z.-]*$)"));
    if (!safeId.match(providerId).hasMatch() || !safeVersion.match(version).hasMatch()) {
        setError(tr("Provider 或版本号无效"));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }
    const bool removingDefault = m_defaultVersions.value(providerId).toString() == version;
    if (removingDefault && !deactivateProvider(providerId)) {
        setError(tr("无法移除 %1 %2 的系统 PATH 指向").arg(providerId, version));
        return;
    }

    const QString downloadsRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/downloads/");
    const QString installsRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/installs/");
    const QString downloadPath = QDir::cleanPath(downloadDirectory(providerId, version));
    const QString installPath = QDir::cleanPath(installDirectory(providerId, version));
    if (!downloadPath.startsWith(QDir::cleanPath(downloadsRoot), Qt::CaseInsensitive)
        || !installPath.startsWith(QDir::cleanPath(installsRoot), Qt::CaseInsensitive)) {
        setError(tr("拒绝删除不安全的目录"));
        return;
    }

    if (m_providerId != providerId) {
        m_providerId = providerId;
        emit activeProviderChanged();
    }
    if (m_version != version) {
        m_version = version;
        emit activeVersionChanged();
    }
    setError({});
    setStatus(tr("正在删除 %1 %2…").arg(providerId, version));
    setProgress(0.0);
    m_removing = true;
    emit removingChanged();
    setBusy(true);

    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, providerId, version, removingDefault] {
        const bool removed = watcher->result();
        watcher->deleteLater();
        m_removing = false;
        emit removingChanged();
        setBusy(false);
        if (!removed) {
            setError(tr("无法完整删除 %1 %2，请检查文件是否被占用").arg(providerId, version));
            setStatus({});
            return;
        }
        setProgress(1.0);
        setError({});
        setStatus(removingDefault
                      ? tr("%1 %2 及其系统 PATH 指向已删除").arg(providerId, version)
                      : tr("%1 %2 已删除").arg(providerId, version));
        emit downloadRemoved(providerId, version);
    });
    watcher->setFuture(QtConcurrent::run([downloadPath, installPath] {
        QDir downloadDir(downloadPath);
        QDir installDir(installPath);
        const bool downloadRemoved =
            !downloadDir.exists() || downloadDir.removeRecursively();
        const bool installRemoved =
            !installDir.exists() || installDir.removeRecursively();
        return downloadRemoved && installRemoved;
    }));
}

void ProviderController::cancel()
{
    if (m_reply) {
        m_reply->abort();
    }
    if (m_installProcess.state() != QProcess::NotRunning) {
        m_installProcess.kill();
    }
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_javaReplies)) {
        if (reply)
            reply->abort();
    }
}

void ProviderController::fetchNodeChecksum()
{
    setStatus(tr("正在获取官方 SHA-256 校验清单…"));
    const QUrl url(QString::fromLatin1(nodeDistBase) + u'v' + m_version + QStringLiteral("/SHASUMS256.txt"));
    m_reply = m_network.get(QNetworkRequest(url));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            QFile::remove(m_downloadFile.fileName());
            fail(tr("获取校验清单失败：%1").arg(message));
            return;
        }
        const QByteArray checksumDocument = reply->readAll();
        reply->deleteLater();
        verifyNodeDownload(checksumDocument);
    });
}

void ProviderController::verifyNodeDownload(const QByteArray &checksumDocument)
{
    const QByteArray fileName = nodeFileName(m_version).toUtf8();
    QByteArray expectedHash;
    const QList<QByteArray> lines = checksumDocument.split('\n');
    for (const QByteArray &line : lines) {
        if (line.trimmed().endsWith(fileName)) {
            expectedHash = line.left(64).toLower();
            break;
        }
    }
    if (expectedHash.size() != 64) {
        QFile::remove(m_downloadFile.fileName());
        fail(tr("官方校验清单中没有找到目标文件"));
        return;
    }
    verifyDownloadedFile(expectedHash);
}

void ProviderController::fetchMavenChecksum()
{
    setStatus(tr("正在获取 Apache 官方 SHA-512 校验值…"));
    QUrl checksumUrl;
    for (const QVariant &entry : std::as_const(m_versions)) {
        const QVariantMap release = entry.toMap();
        if (release.value(QStringLiteral("version")).toString() == m_version) {
            checksumUrl = QUrl(release.value(QStringLiteral("downloadUrl")).toString()
                               + QStringLiteral(".sha512"));
            break;
        }
    }
    if (!checksumUrl.isValid()) {
        QFile::remove(m_downloadFile.fileName());
        fail(tr("找不到 Maven 官方 SHA-512 校验地址"));
        return;
    }
    m_reply = m_network.get(QNetworkRequest(checksumUrl));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            QFile::remove(m_downloadFile.fileName());
            fail(tr("获取 Maven SHA-512 校验值失败：%1").arg(message));
            return;
        }
        const QByteArray document = reply->readAll();
        reply->deleteLater();
        verifyMavenDownload(document);
    });
}

void ProviderController::verifyMavenDownload(const QByteArray &checksumDocument)
{
    const QByteArray expectedHash = checksumDocument.trimmed().left(128).toLower();
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{128}$"));
    if (!hashPattern.match(QString::fromLatin1(expectedHash)).hasMatch()) {
        QFile::remove(m_downloadFile.fileName());
        fail(tr("Maven 官方 SHA-512 校验值格式无效"));
        return;
    }
    verifyDownloadedFile(expectedHash);
}

void ProviderController::verifyDownloadedFile(const QByteArray &expectedHash)
{
    QFile file(m_downloadFile.fileName());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(tr("无法读取下载文件进行校验"));
        return;
    }
    const bool sha512 = expectedHash.size() == 128;
    QCryptographicHash hash(sha512 ? QCryptographicHash::Sha512
                                  : QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        file.close();
        fail(tr("无法计算下载文件的 SHA-256"));
        return;
    }
    file.close();

    if (hash.result().toHex().toLower() != expectedHash) {
        QFile::remove(m_downloadFile.fileName());
        fail(tr("SHA-256 校验失败，下载文件已删除"));
        return;
    }

    QFile::remove(m_downloadPath);
    if (!QFile::rename(m_downloadFile.fileName(), m_downloadPath)) {
        fail(tr("校验成功，但无法完成文件重命名"));
        return;
    }

    QJsonObject manifestObject;
    manifestObject.insert(QStringLiteral("schemaVersion"), 1);
    manifestObject.insert(QStringLiteral("provider"), m_providerId);
    manifestObject.insert(QStringLiteral("version"), m_version);
    manifestObject.insert(QStringLiteral("file"), QFileInfo(m_downloadPath).fileName());
    manifestObject.insert(sha512 ? QStringLiteral("sha512") : QStringLiteral("sha256"),
                          QString::fromLatin1(expectedHash));
    manifestObject.insert(QStringLiteral("verification"),
                          m_verificationMode == QStringLiteral("unverified")
                              ? QStringLiteral("local-integrity-only")
                          : m_verificationMode == QStringLiteral("authenticode")
                              ? QStringLiteral("authenticode")
                               : sha512 ? QStringLiteral("official-sha512")
                                        : QStringLiteral("official-sha256"));
    QSaveFile manifest(downloadManifestPath(m_providerId, m_version));
    if (manifest.open(QIODevice::WriteOnly)) {
        manifest.write(QJsonDocument(manifestObject).toJson(QJsonDocument::Indented));
        manifest.commit();
    }

    setProgress(1.0);
    setStatus(tr("%1 %2 下载并校验完成")
                  .arg(m_providerId == QStringLiteral("node") ? QStringLiteral("Node.js")
                       : m_providerId == QStringLiteral("flutter") ? QStringLiteral("Flutter")
                       : m_providerId == QStringLiteral("java") ? QStringLiteral("Java")
                       : m_providerId == QStringLiteral("python") ? QStringLiteral("Python")
                       : m_providerId == QStringLiteral("php") ? QStringLiteral("PHP")
                       : m_providerId == QStringLiteral("go") ? QStringLiteral("Go")
                       : m_providerId == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                                                                : QStringLiteral("PostgreSQL"),
                       m_version));
    setBusy(false);
    emit downloadFinished(m_providerId, m_version, m_downloadPath);
}

void ProviderController::recordUnverifiedDownload()
{
    const QString providerName = m_providerId == QStringLiteral("postgresql")
        ? QStringLiteral("PostgreSQL")
        : QStringLiteral("PHP");
    QFile file(m_downloadFile.fileName());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(tr("无法读取 %1 下载包以记录本地完整性").arg(providerName));
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        fail(tr("无法计算 %1 下载包的本地 SHA-256").arg(providerName));
        return;
    }
    setStatus(tr("%1 下载包没有可自动校验的摘要；正在记录本地完整性基线…")
                  .arg(providerName));
    verifyDownloadedFile(hash.result().toHex());
}

void ProviderController::verifyPythonDownload()
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(m_downloadFile.fileName());
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = reinterpret_cast<LPCWSTR>(nativePath.utf16());

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    const LONG result = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    if (result != ERROR_SUCCESS) {
        QFile::remove(m_downloadFile.fileName());
        fail(tr("Python 官方安装程序的 Authenticode 签名验证失败（错误 0x%1）")
                 .arg(qulonglong(result), 8, 16, QLatin1Char('0')));
        return;
    }

    // The embedded signature authenticates the complete executable. Hash it as
    // well so the local manifest can detect later corruption without network access.
    QFile file(m_downloadFile.fileName());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(tr("无法读取 Python 安装程序"));
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        fail(tr("无法计算 Python 安装程序的 SHA-256"));
        return;
    }
    verifyDownloadedFile(hash.result().toHex());
#else
    QFile::remove(m_downloadFile.fileName());
    fail(tr("当前平台不支持 Python 安装程序签名验证"));
#endif
}

QString ProviderController::nodeFileName(const QString &version) const
{
    return QStringLiteral("node-v%1-win-x64.zip").arg(version);
}

QString ProviderController::defaultsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/settings/default-versions.json");
}

QString ProviderController::installDirectory(const QString &providerId, const QString &version) const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/installs/") + providerId + u'/' + version;
}

QString ProviderController::shimDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/bin");
}

void ProviderController::installMaven(const QString &version)
{
    const QString destination = installDirectory(QStringLiteral("maven"), version);
    if (QFileInfo(destination + QStringLiteral("/bin/mvn.cmd")).isFile()) {
        if (m_makeDefaultAfterInstall && !activateMaven(version))
            setError(tr("无法激活 Apache Maven %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }
    QFile manifest(downloadManifestPath(QStringLiteral("maven"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("Maven %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive = downloadDirectory(QStringLiteral("maven"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("Maven %1 下载文件不存在").arg(version));
        return;
    }
    const QString installsRoot = QFileInfo(destination).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if ((staging.exists() && !staging.removeRecursively())
        || !QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 Maven 安装暂存目录"));
        return;
    }
    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("maven");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 Maven %1…").arg(version));
    setBusy(true);
    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        QDir staging(m_pendingStagingPath);
        const QStringList roots = staging.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (exitStatus != QProcess::NormalExit || exitCode != 0 || roots.size() != 1
            || !QFileInfo(staging.filePath(roots.first() + QStringLiteral("/bin/mvn.cmd"))).isFile()) {
            const QString details = QString::fromLocal8Bit(
                m_installProcess.readAllStandardError()).trimmed();
            staging.removeRecursively();
            fail(tr("解压 Maven 失败：%1").arg(details));
            return;
        }
        const QString destination =
            installDirectory(QStringLiteral("maven"), m_pendingInstallVersion);
        if (QFileInfo(destination).exists()
            || !QDir().rename(staging.filePath(roots.first()), destination)) {
            staging.removeRecursively();
            fail(tr("无法完成 Maven 安装目录切换"));
            return;
        }
        staging.removeRecursively();
        if (m_makeDefaultAfterInstall && !activateMaven(m_pendingInstallVersion)) {
            fail(tr("Maven 已解压，但写入系统 PATH 失败"));
            return;
        }
        setStatus(tr("Maven %1 安装完成").arg(m_pendingInstallVersion));
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"),
                            m_pendingStagingPath});
}

bool ProviderController::activateMaven(const QString &version)
{
    const QString bin = installDirectory(QStringLiteral("maven"), version)
        + QStringLiteral("/bin");
    if (!writeCommandShim(QStringLiteral("mvn"), bin + QStringLiteral("/mvn.cmd"))
        || !writeCommandShim(QStringLiteral("mvnDebug"), bin + QStringLiteral("/mvnDebug.cmd"))
        || !ensureShimPath())
        return false;
    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("maven"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivateNode(const QString &version)
{
    const QString installedNode = installDirectory(QStringLiteral("node"), version)
        + QStringLiteral("/node.exe");
    if (QFileInfo(installedNode).isFile()) {
        if (m_makeDefaultAfterInstall && !activateNode(version)) {
            setError(tr("无法激活 Node.js %1").arg(version));
        }
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    const QString archive = downloadDirectory(QStringLiteral("node"), version)
        + u'/' + nodeFileName(version);
    if (!QFileInfo(archive).isFile()) {
        setError(tr("Node.js %1 下载文件不存在").arg(version));
        return;
    }

    const QString installsRoot = QFileInfo(installDirectory(QStringLiteral("node"), version)).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 SDK 安装目录"));
        return;
    }

    m_pendingInstallVersion = version;
    if (m_providerId != QStringLiteral("node")) {
        m_providerId = QStringLiteral("node");
        emit activeProviderChanged();
    }
    if (m_version != version) {
        m_version = version;
        emit activeVersionChanged();
    }
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 Node.js %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy) {
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details = QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 Node.js 失败：%1").arg(details));
            return;
        }

        const QString extracted = m_pendingStagingPath + QStringLiteral("/node-v")
            + m_pendingInstallVersion + QStringLiteral("-win-x64");
        const QString destination = installDirectory(QStringLiteral("node"), m_pendingInstallVersion);
        if (!QFileInfo(extracted).isDir()) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("Node.js 压缩包目录结构无效"));
            return;
        }
        if (QFileInfo(destination).exists() || !QDir().rename(extracted, destination)) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法完成 Node.js 安装目录切换"));
            return;
        }
        QDir(m_pendingStagingPath).removeRecursively();

        if (m_makeDefaultAfterInstall && !activateNode(m_pendingInstallVersion)) {
            fail(tr("Node.js 已解压，但写入系统 PATH 失败"));
            return;
        }
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"), m_pendingStagingPath});
}

bool ProviderController::activateNode(const QString &version)
{
    const QString root = installDirectory(QStringLiteral("node"), version);
    if (!writeCommandShim(QStringLiteral("node"), root + QStringLiteral("/node.exe"))
        || !writeCommandShim(QStringLiteral("npm"), root + QStringLiteral("/npm.cmd"))
        || !writeCommandShim(QStringLiteral("npx"), root + QStringLiteral("/npx.cmd"))
        || !ensureShimPath()) {
        return false;
    }

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("node"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("Node.js %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivateFlutter(const QString &version)
{
    const QString flutterBat = installDirectory(QStringLiteral("flutter"), version)
        + QStringLiteral("/bin/flutter.bat");
    if (QFileInfo(flutterBat).isFile()) {
        if (m_makeDefaultAfterInstall && !activateFlutter(version))
            setError(tr("无法激活 Flutter %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    QFile manifest(downloadManifestPath(QStringLiteral("flutter"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("Flutter %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive = downloadDirectory(QStringLiteral("flutter"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("Flutter %1 下载文件不存在").arg(version));
        return;
    }

    const QString installsRoot =
        QFileInfo(installDirectory(QStringLiteral("flutter"), version)).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的 Flutter 解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 Flutter 安装目录"));
        return;
    }

    m_pendingInstallVersion = version;
    if (m_providerId != QStringLiteral("flutter")) {
        m_providerId = QStringLiteral("flutter");
        emit activeProviderChanged();
    }
    if (m_version != version) {
        m_version = version;
        emit activeVersionChanged();
    }
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 Flutter %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details =
                QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 Flutter 失败：%1").arg(details));
            return;
        }

        const QString extracted = m_pendingStagingPath + QStringLiteral("/flutter");
        const QString destination =
            installDirectory(QStringLiteral("flutter"), m_pendingInstallVersion);
        if (!QFileInfo(extracted).isDir()) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("Flutter 压缩包目录结构无效"));
            return;
        }
        if (QFileInfo(destination).exists() || !QDir().rename(extracted, destination)) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法完成 Flutter 安装目录切换"));
            return;
        }
        QDir(m_pendingStagingPath).removeRecursively();

        if (m_makeDefaultAfterInstall && !activateFlutter(m_pendingInstallVersion)) {
            fail(tr("Flutter 已解压，但写入系统 PATH 失败"));
            return;
        }
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"), m_pendingStagingPath});
}

bool ProviderController::activateFlutter(const QString &version)
{
    const QString bin = installDirectory(QStringLiteral("flutter"), version)
        + QStringLiteral("/bin");
    if (!writeCommandShim(QStringLiteral("flutter"), bin + QStringLiteral("/flutter.bat"))
        || !writeCommandShim(QStringLiteral("dart"), bin + QStringLiteral("/dart.bat"))
        || !ensureShimPath()) {
        return false;
    }

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("flutter"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("Flutter %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivateJava(const QString &version)
{
    const QString installedJava = installDirectory(QStringLiteral("java"), version)
        + QStringLiteral("/bin/java.exe");
    if (QFileInfo(installedJava).isFile()) {
        if (m_makeDefaultAfterInstall && !activateJava(version))
            setError(tr("无法激活 Java %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    QFile manifest(downloadManifestPath(QStringLiteral("java"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("Java %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive = downloadDirectory(QStringLiteral("java"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("Java %1 下载文件不存在").arg(version));
        return;
    }

    const QString installsRoot =
        QFileInfo(installDirectory(QStringLiteral("java"), version)).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的 Java 解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 Java 安装目录"));
        return;
    }

    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("java");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 Java %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details =
                QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 Java 失败：%1").arg(details));
            return;
        }

        QDir staging(m_pendingStagingPath);
        const QStringList roots = staging.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (roots.size() != 1
            || !QFileInfo(staging.filePath(roots.first() + QStringLiteral("/bin/java.exe"))).isFile()) {
            staging.removeRecursively();
            fail(tr("Java 压缩包目录结构无效"));
            return;
        }
        const QString destination =
            installDirectory(QStringLiteral("java"), m_pendingInstallVersion);
        if (QFileInfo(destination).exists()
            || !QDir().rename(staging.filePath(roots.first()), destination)) {
            staging.removeRecursively();
            fail(tr("无法完成 Java 安装目录切换"));
            return;
        }
        staging.removeRecursively();

        if (m_makeDefaultAfterInstall && !activateJava(m_pendingInstallVersion)) {
            fail(tr("Java 已解压，但写入系统 PATH 失败"));
            return;
        }
        setStatus(tr("Java %1 安装完成").arg(m_pendingInstallVersion));
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"),
                            m_pendingStagingPath});
}

bool ProviderController::activateJava(const QString &version)
{
    const QString bin = installDirectory(QStringLiteral("java"), version)
        + QStringLiteral("/bin");
    const QStringList commands{QStringLiteral("java"), QStringLiteral("javac"),
                               QStringLiteral("jar"), QStringLiteral("javadoc"),
                               QStringLiteral("jshell")};
    for (const QString &command : commands) {
        if (!writeCommandShim(command, bin + u'/' + command + QStringLiteral(".exe")))
            return false;
    }
    if (!ensureShimPath())
        return false;

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("java"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("Java %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivatePython(const QString &version)
{
    const QString pythonExe = installDirectory(QStringLiteral("python"), version)
        + QStringLiteral("/python.exe");
    if (QFileInfo(pythonExe).isFile()) {
        if (m_makeDefaultAfterInstall && !activatePython(version))
            setError(tr("无法激活 Python %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    QFile manifest(downloadManifestPath(QStringLiteral("python"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("Python %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString installer =
        downloadDirectory(QStringLiteral("python"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(installer).isFile()) {
        setError(tr("Python %1 安装程序不存在").arg(version));
        return;
    }

    const QString destination = installDirectory(QStringLiteral("python"), version);
    if (QFileInfo(destination).exists()) {
        setError(tr("Python %1 安装目录已存在，请先修复或删除该版本").arg(version));
        return;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        setError(tr("无法创建 Python 安装根目录"));
        return;
    }

    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("python");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在安装 Python %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("无法启动 Python 官方安装程序"));
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        const QString version = m_pendingInstallVersion;
        const QString destination = installDirectory(QStringLiteral("python"), version);
        if (exitStatus != QProcess::NormalExit || exitCode != 0
            || !QFileInfo(destination + QStringLiteral("/python.exe")).isFile()) {
            fail(tr("Python %1 安装失败（退出码 %2）").arg(version).arg(exitCode));
            return;
        }
        if (m_makeDefaultAfterInstall && !activatePython(version)) {
            fail(tr("Python 已安装，但写入系统 PATH 失败"));
            return;
        }
        setStatus(tr("Python %1 安装完成").arg(version));
        setProgress(1.0);
        setBusy(false);
    });

    m_installProcess.start(
        installer,
        {QStringLiteral("/quiet"), QStringLiteral("InstallAllUsers=0"),
         QStringLiteral("TargetDir=%1").arg(QDir::toNativeSeparators(destination)),
         QStringLiteral("Include_launcher=0"), QStringLiteral("Include_pip=1"),
         QStringLiteral("Include_tcltk=1"), QStringLiteral("Include_test=0"),
         QStringLiteral("Include_doc=0"), QStringLiteral("AssociateFiles=0"),
         QStringLiteral("Shortcuts=0"), QStringLiteral("PrependPath=0"),
         QStringLiteral("AppendPath=0")});
}

bool ProviderController::activatePython(const QString &version)
{
    const QString root = installDirectory(QStringLiteral("python"), version);
    if (!writeCommandShim(QStringLiteral("python"), root + QStringLiteral("/python.exe"))
        || !writeCommandShim(QStringLiteral("pip"), root + QStringLiteral("/Scripts/pip.exe"))
        || !ensureShimPath()) {
        return false;
    }

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("python"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("Python %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivatePhp(const QString &version)
{
    const QString phpExe = installDirectory(QStringLiteral("php"), version)
        + QStringLiteral("/php.exe");
    if (QFileInfo(phpExe).isFile()) {
        if (m_makeDefaultAfterInstall && !activatePhp(version))
            setError(tr("无法激活 PHP %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    QFile manifest(downloadManifestPath(QStringLiteral("php"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("PHP %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive = downloadDirectory(QStringLiteral("php"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("PHP %1 下载文件不存在").arg(version));
        return;
    }

    const QString installsRoot =
        QFileInfo(installDirectory(QStringLiteral("php"), version)).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的 PHP 解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 PHP 安装目录"));
        return;
    }

    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("php");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 PHP %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details =
                QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 PHP 失败：%1").arg(details));
            return;
        }
        if (!QFileInfo(m_pendingStagingPath + QStringLiteral("/php.exe")).isFile()) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("PHP 压缩包目录结构无效"));
            return;
        }
        const QString developmentIni =
            m_pendingStagingPath + QStringLiteral("/php.ini-development");
        const QString activeIni = m_pendingStagingPath + QStringLiteral("/php.ini");
        if (!QFileInfo(developmentIni).isFile()
            || (!QFileInfo(activeIni).exists() && !QFile::copy(developmentIni, activeIni))) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法生成 PHP 的受管理 php.ini"));
            return;
        }

        const QString destination =
            installDirectory(QStringLiteral("php"), m_pendingInstallVersion);
        if (QFileInfo(destination).exists()
            || !QDir().rename(m_pendingStagingPath, destination)) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法完成 PHP 安装目录切换"));
            return;
        }
        if (m_makeDefaultAfterInstall && !activatePhp(m_pendingInstallVersion)) {
            fail(tr("PHP 已解压，但写入系统 PATH 失败"));
            return;
        }
        setStatus(tr("PHP %1 安装完成").arg(m_pendingInstallVersion));
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"),
                            m_pendingStagingPath});
}

bool ProviderController::activatePhp(const QString &version)
{
    const QString root = installDirectory(QStringLiteral("php"), version);
    if (!writeCommandShim(QStringLiteral("php"), root + QStringLiteral("/php.exe"))
        || !ensureShimPath()) {
        return false;
    }

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("php"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("PHP %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installAndActivateGo(const QString &version)
{
    const QString goExe =
        installDirectory(QStringLiteral("go"), version) + QStringLiteral("/bin/go.exe");
    if (QFileInfo(goExe).isFile()) {
        if (m_makeDefaultAfterInstall && !activateGo(version))
            setError(tr("无法激活 Go %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }

    QFile manifest(downloadManifestPath(QStringLiteral("go"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("Go %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive = downloadDirectory(QStringLiteral("go"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("Go %1 下载文件不存在").arg(version));
        return;
    }

    const QString installsRoot =
        QFileInfo(installDirectory(QStringLiteral("go"), version)).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的 Go 解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 Go 安装目录"));
        return;
    }

    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("go");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 Go %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details =
                QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 Go 失败：%1").arg(details));
            return;
        }
        const QString extracted = m_pendingStagingPath + QStringLiteral("/go");
        if (!QFileInfo(extracted + QStringLiteral("/bin/go.exe")).isFile()) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("Go 压缩包目录结构无效"));
            return;
        }
        const QString destination =
            installDirectory(QStringLiteral("go"), m_pendingInstallVersion);
        if (QFileInfo(destination).exists() || !QDir().rename(extracted, destination)) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法完成 Go 安装目录切换"));
            return;
        }
        QDir(m_pendingStagingPath).removeRecursively();
        if (m_makeDefaultAfterInstall && !activateGo(m_pendingInstallVersion)) {
            fail(tr("Go 已解压，但写入系统 PATH 失败"));
            return;
        }
        setStatus(tr("Go %1 安装完成").arg(m_pendingInstallVersion));
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"),
                            m_pendingStagingPath});
}

bool ProviderController::activateGo(const QString &version)
{
    const QString bin =
        installDirectory(QStringLiteral("go"), version) + QStringLiteral("/bin");
    if (!writeCommandShim(QStringLiteral("go"), bin + QStringLiteral("/go.exe"))
        || !writeCommandShim(QStringLiteral("gofmt"), bin + QStringLiteral("/gofmt.exe"))
        || !ensureShimPath()) {
        return false;
    }
    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("go"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("Go %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

void ProviderController::installPostgresql(const QString &version)
{
    const QString destination = installDirectory(QStringLiteral("postgresql"), version);
    if (QFileInfo(destination + QStringLiteral("/bin/psql.exe")).isFile()) {
        if (m_makeDefaultAfterInstall && !activatePostgresql(version))
            setError(tr("无法激活 PostgreSQL %1").arg(version));
        return;
    }
    if (m_busy) {
        setError(tr("已有任务正在执行"));
        return;
    }
    QFile manifest(downloadManifestPath(QStringLiteral("postgresql"), version));
    if (!manifest.open(QIODevice::ReadOnly)) {
        setError(tr("PostgreSQL %1 下载记录不存在").arg(version));
        return;
    }
    const QString fileName = QJsonDocument::fromJson(manifest.readAll())
                                 .object().value(QStringLiteral("file")).toString();
    const QString archive =
        downloadDirectory(QStringLiteral("postgresql"), version) + u'/' + fileName;
    if (fileName.isEmpty() || !QFileInfo(archive).isFile()) {
        setError(tr("PostgreSQL %1 下载文件不存在").arg(version));
        return;
    }
    const QString installsRoot = QFileInfo(destination).absolutePath();
    m_pendingStagingPath = installsRoot + QStringLiteral("/.extracting-") + version;
    QDir staging(m_pendingStagingPath);
    if (staging.exists() && !staging.removeRecursively()) {
        setError(tr("无法清理上次未完成的 PostgreSQL 解压目录"));
        return;
    }
    if (!QDir().mkpath(m_pendingStagingPath)) {
        setError(tr("无法创建 PostgreSQL 安装目录"));
        return;
    }
    m_pendingInstallVersion = version;
    m_providerId = QStringLiteral("postgresql");
    m_version = version;
    emit activeProviderChanged();
    emit activeVersionChanged();
    setError({});
    setProgress(0.0);
    setStatus(tr("正在解压并安装 PostgreSQL %1…").arg(version));
    setBusy(true);

    m_installProcess.disconnect(this);
    connect(&m_installProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("无法启动系统解压工具 tar.exe"));
        }
    });
    connect(&m_installProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_busy)
            return;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString details =
                QString::fromLocal8Bit(m_installProcess.readAllStandardError()).trimmed();
            QDir(m_pendingStagingPath).removeRecursively();
            fail(tr("解压 PostgreSQL 失败：%1").arg(details));
            return;
        }
        QDir staging(m_pendingStagingPath);
        QString extracted = m_pendingStagingPath;
        if (!QFileInfo(extracted + QStringLiteral("/bin/psql.exe")).isFile()) {
            const QStringList roots = staging.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            if (roots.size() == 1)
                extracted = staging.filePath(roots.first());
        }
        if (!QFileInfo(extracted + QStringLiteral("/bin/psql.exe")).isFile()) {
            staging.removeRecursively();
            fail(tr("PostgreSQL 压缩包目录结构无效"));
            return;
        }
        const QString destination =
            installDirectory(QStringLiteral("postgresql"), m_pendingInstallVersion);
        if (QFileInfo(destination).exists()
            || !QDir().rename(extracted, destination)) {
            staging.removeRecursively();
            fail(tr("无法完成 PostgreSQL 安装目录切换"));
            return;
        }
        staging.removeRecursively();
        if (m_makeDefaultAfterInstall
            && !activatePostgresql(m_pendingInstallVersion)) {
            fail(tr("PostgreSQL 已解压，但写入系统 PATH 指向失败"));
            return;
        }
        setStatus(tr("PostgreSQL %1 程序安装完成；创建实例后才能启动服务")
                      .arg(m_pendingInstallVersion));
        setProgress(1.0);
        setBusy(false);
    });
    m_installProcess.start(QStringLiteral("tar.exe"),
                           {QStringLiteral("-xf"), archive, QStringLiteral("-C"),
                            m_pendingStagingPath});
}

bool ProviderController::activatePostgresql(const QString &version)
{
    const QString bin =
        installDirectory(QStringLiteral("postgresql"), version) + QStringLiteral("/bin");
    const QStringList commands{
        QStringLiteral("psql"),       QStringLiteral("postgres"),
        QStringLiteral("pg_ctl"),     QStringLiteral("initdb"),
        QStringLiteral("createdb"),   QStringLiteral("dropdb"),
        QStringLiteral("createuser"), QStringLiteral("dropuser"),
        QStringLiteral("pg_dump"),    QStringLiteral("pg_restore"),
        QStringLiteral("pg_isready")};
    for (const QString &command : commands) {
        if (!writeCommandShim(command, bin + u'/' + command + QStringLiteral(".exe")))
            return false;
    }
    if (!ensureShimPath())
        return false;

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.insert(QStringLiteral("postgresql"), version);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }
    setError({});
    setStatus(tr("PostgreSQL %1 已设为系统默认版本；新终端中生效").arg(version));
    emit defaultVersionsChanged();
    return true;
}

bool ProviderController::deactivateProvider(const QString &providerId)
{
    QStringList shimNames;
    if (providerId == QStringLiteral("node"))
        shimNames = {QStringLiteral("node"), QStringLiteral("npm"), QStringLiteral("npx")};
    else if (providerId == QStringLiteral("flutter"))
        shimNames = {QStringLiteral("flutter"), QStringLiteral("dart")};
    else if (providerId == QStringLiteral("java"))
        shimNames = {QStringLiteral("java"), QStringLiteral("javac"), QStringLiteral("jar"),
                     QStringLiteral("javadoc"), QStringLiteral("jshell")};
    else if (providerId == QStringLiteral("python"))
        shimNames = {QStringLiteral("python"), QStringLiteral("pip")};
    else if (providerId == QStringLiteral("php"))
        shimNames = {QStringLiteral("php")};
    else if (providerId == QStringLiteral("go"))
        shimNames = {QStringLiteral("go"), QStringLiteral("gofmt")};
    else if (providerId == QStringLiteral("postgresql"))
        shimNames = {QStringLiteral("psql"), QStringLiteral("postgres"),
                     QStringLiteral("pg_ctl"), QStringLiteral("initdb"),
                     QStringLiteral("createdb"), QStringLiteral("dropdb"),
                     QStringLiteral("createuser"), QStringLiteral("dropuser"),
                     QStringLiteral("pg_dump"), QStringLiteral("pg_restore"),
                     QStringLiteral("pg_isready")};
    else if (providerId == QStringLiteral("maven"))
        shimNames = {QStringLiteral("mvn"), QStringLiteral("mvnDebug")};
    else
        return false;

    const QVariantMap previous = m_defaultVersions;
    m_defaultVersions.remove(providerId);
    if (!saveDefaults()) {
        m_defaultVersions = previous;
        return false;
    }

    bool removed = true;
    for (const QString &name : std::as_const(shimNames)) {
        const QString path = shimDirectory() + u'/' + name + QStringLiteral(".cmd");
        if (QFileInfo(path).exists() && !QFile::remove(path))
            removed = false;
    }
    emit defaultVersionsChanged();
    return removed;
}

bool ProviderController::writeCommandShim(const QString &name, const QString &target)
{
    if (!QFileInfo(target).isFile() || !QDir().mkpath(shimDirectory())) {
        return false;
    }
    QSaveFile shim(shimDirectory() + u'/' + name + QStringLiteral(".cmd"));
    if (!shim.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray content = QByteArrayLiteral("@echo off\r\ncall \"")
        + QDir::toNativeSeparators(target).toLocal8Bit() + QByteArrayLiteral("\" %*\r\n");
    shim.write(content);
    return shim.commit();
}

bool ProviderController::ensureShimPath()
{
#ifdef Q_OS_WIN
    QSettings environment(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    const QString path = environment.value(QStringLiteral("Path")).toString();
    const QString nativeShim = QDir::toNativeSeparators(shimDirectory());
    const QStringList entries = path.split(u';', Qt::SkipEmptyParts);
    QStringList reorderedEntries;
    reorderedEntries.append(nativeShim);
    for (const QString &entry : entries) {
        if (QDir::cleanPath(entry).compare(QDir::cleanPath(nativeShim), Qt::CaseInsensitive) != 0) {
            reorderedEntries.append(entry);
        }
    }
    environment.setValue(QStringLiteral("Path"), reorderedEntries.join(u';'));
    environment.sync();
    if (environment.status() != QSettings::NoError) {
        return false;
    }
    QSettings verification(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    if (!verification.value(QStringLiteral("Path")).toString().startsWith(nativeShim,
                                                                           Qt::CaseInsensitive)) {
        return false;
    }

    QString processPath = QString::fromLocal8Bit(qgetenv("PATH"));
    if (!processPath.contains(nativeShim, Qt::CaseInsensitive)) {
        processPath = nativeShim + u';' + processPath;
        qputenv("PATH", processPath.toLocal8Bit());
    }
    SendMessageTimeoutW(HWND_BROADCAST,
                        WM_SETTINGCHANGE,
                        0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG,
                        2000,
                        nullptr);
    return true;
#else
    return false;
#endif
}

void ProviderController::loadDefaults()
{
    QFile file(defaultsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject providers = object.value(QStringLiteral("providers")).toObject();
    for (auto it = providers.constBegin(); it != providers.constEnd(); ++it) {
        if (it.value().isString()) {
            m_defaultVersions.insert(it.key(), it.value().toString());
        }
    }

#ifdef Q_OS_WIN
    const QString nodeVersion = m_defaultVersions.value(QStringLiteral("node")).toString();
    if (!nodeVersion.isEmpty()) {
        const QString expectedRoot = QDir::toNativeSeparators(
            installDirectory(QStringLiteral("node"), nodeVersion));
        QFile shim(shimDirectory() + QStringLiteral("/node.cmd"));
        const bool validInstall = QFileInfo(expectedRoot + QStringLiteral("\\node.exe")).isFile();
        const bool validShim = shim.open(QIODevice::ReadOnly)
            && QString::fromLocal8Bit(shim.readAll()).contains(expectedRoot, Qt::CaseInsensitive);
        QSettings environment(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
        const QString userPath = environment.value(QStringLiteral("Path")).toString();
        const bool validPath = userPath.startsWith(
            QDir::toNativeSeparators(shimDirectory()), Qt::CaseInsensitive);
        if (!validInstall || !validShim || !validPath) {
            m_defaultVersions.remove(QStringLiteral("node"));
            saveDefaults();
        }
    }
#endif
}

bool ProviderController::saveDefaults()
{
    QJsonObject providers;
    for (auto it = m_defaultVersions.constBegin(); it != m_defaultVersions.constEnd(); ++it) {
        providers.insert(it.key(), it.value().toString());
    }
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("providers"), providers);

    const QFileInfo info(defaultsPath());
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QSaveFile file(defaultsPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

void ProviderController::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
    if (!busy && m_settingDefault) {
        m_settingDefault = false;
        emit settingDefaultChanged();
    }
}

void ProviderController::setProgress(double progress)
{
    if (qFuzzyCompare(m_progress, progress)) return;
    m_progress = progress;
    emit progressChanged();
}

void ProviderController::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged();
}

void ProviderController::setError(const QString &error)
{
    if (m_error == error) return;
    m_error = error;
    emit errorChanged();
}

void ProviderController::fail(const QString &message)
{
    setError(message);
    setStatus({});
    setBusy(false);
}
