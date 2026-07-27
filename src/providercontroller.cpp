#include "providercontroller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr auto nodeIndexUrl = "https://nodejs.org/dist/index.json";
constexpr auto nodeDistBase = "https://nodejs.org/dist/";
}

ProviderController::ProviderController(QObject *parent)
    : QObject(parent)
{
    loadDefaults();
}

QVariantList ProviderController::versions() const { return m_versions; }
bool ProviderController::busy() const { return m_busy; }
double ProviderController::progress() const { return m_progress; }
QString ProviderController::status() const { return m_status; }
QString ProviderController::error() const { return m_error; }
QString ProviderController::activeProvider() const { return m_providerId; }
QString ProviderController::activeVersion() const { return m_version; }
QVariantMap ProviderController::defaultVersions() const { return m_defaultVersions; }

void ProviderController::loadVersions(const QString &providerId, bool forceRefresh)
{
    if (providerId != QStringLiteral("node")) {
        setError(tr("%1 Provider 尚未接入官方版本源").arg(providerId));
        return;
    }
    if (m_busy) {
        return;
    }
    if (m_providerId != providerId) {
        m_providerId = providerId;
        emit activeProviderChanged();
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
        if (cache.open(QIODevice::ReadOnly) && applyNodeIndex(cache.readAll())) {
            setStatus(tr("已读取本地 Node.js 版本缓存"));
            return;
        }
    }

    setError({});
    setStatus(tr("正在从 Node.js 官方源获取版本…"));
    setProgress(0.0);
    setBusy(true);

    m_reply = m_network.get(QNetworkRequest(QUrl(QString::fromLatin1(nodeIndexUrl))));
    connect(m_reply, &QNetworkReply::finished, this, [this, providerId] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            fail(tr("获取 Node.js 版本失败：%1").arg(message));
            return;
        }

        const QByteArray data = reply->readAll();
        reply->deleteLater();
        if (!applyNodeIndex(data)) {
            fail(tr("Node.js 版本索引格式无效"));
            return;
        }

        QSaveFile cache(cachePath(providerId));
        const QFileInfo cacheInfo(cachePath(providerId));
        QDir().mkpath(cacheInfo.absolutePath());
        if (cache.open(QIODevice::WriteOnly)) {
            cache.write(data);
            cache.commit();
        }

        setStatus(tr("已从 Node.js 官方源获取 %1 个版本").arg(m_versions.size()));
        setBusy(false);
    });
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

void ProviderController::download(const QString &providerId, const QString &version)
{
    if (providerId != QStringLiteral("node")) {
        setError(tr("Provider %1 尚未接入真实下载").arg(providerId));
        return;
    }
    static const QRegularExpression safeVersion(QStringLiteral(R"(^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$)"));
    if (!safeVersion.match(version).hasMatch()) {
        setError(tr("Node.js 版本号无效"));
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
    m_downloadPath = directory + u'/' + nodeFileName(version);
    m_downloadFile.setFileName(m_downloadPath + QStringLiteral(".part"));
    if (!m_downloadFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(tr("无法写入下载文件：%1").arg(m_downloadFile.errorString()));
        return;
    }

    setError({});
    setProgress(0.0);
    setStatus(tr("正在下载 Node.js %1…").arg(version));
    setBusy(true);

    const QUrl url(QString::fromLatin1(nodeDistBase) + u'v' + version + u'/' + nodeFileName(version));
    m_reply = m_network.get(QNetworkRequest(url));
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_reply) {
            m_downloadFile.write(m_reply->readAll());
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            setProgress(static_cast<double>(received) / static_cast<double>(total));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        m_downloadFile.write(reply->readAll());
        m_downloadFile.close();
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->errorString();
            reply->deleteLater();
            QFile::remove(m_downloadFile.fileName());
            fail(tr("下载失败：%1").arg(message));
            return;
        }
        reply->deleteLater();
        fetchNodeChecksum();
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
    if (!isDownloaded(providerId, version)) {
        setError(tr("必须先下载 %1 %2，才能设为默认版本").arg(providerId, version));
        return;
    }
    if (m_defaultVersions.value(providerId).toString() == version) {
        return;
    }
    if (providerId != QStringLiteral("node")) {
        setError(tr("%1 Provider 尚未实现系统 PATH 激活").arg(providerId));
        return;
    }
    installAndActivateNode(version);
}

void ProviderController::cancel()
{
    if (m_reply) {
        m_reply->abort();
    }
    if (m_installProcess.state() != QProcess::NotRunning) {
        m_installProcess.kill();
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

    QFile file(m_downloadFile.fileName());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(tr("无法读取下载文件进行校验"));
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
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
    manifestObject.insert(QStringLiteral("sha256"), QString::fromLatin1(expectedHash));
    QSaveFile manifest(downloadManifestPath(m_providerId, m_version));
    if (manifest.open(QIODevice::WriteOnly)) {
        manifest.write(QJsonDocument(manifestObject).toJson(QJsonDocument::Indented));
        manifest.commit();
    }

    setProgress(1.0);
    setStatus(tr("Node.js %1 下载并校验完成").arg(m_version));
    setBusy(false);
    emit downloadFinished(m_providerId, m_version, m_downloadPath);
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

void ProviderController::installAndActivateNode(const QString &version)
{
    const QString installedNode = installDirectory(QStringLiteral("node"), version)
        + QStringLiteral("/node.exe");
    if (QFileInfo(installedNode).isFile()) {
        if (!activateNode(version)) {
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

        if (!activateNode(m_pendingInstallVersion)) {
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
