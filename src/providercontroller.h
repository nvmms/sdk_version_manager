#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QVariantList>

class QNetworkReply;
class SvmEventBus;

class ProviderController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList versions READ versions NOTIFY versionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString activeProvider READ activeProvider NOTIFY activeProviderChanged)
    Q_PROPERTY(QString activeVersion READ activeVersion NOTIFY activeVersionChanged)
    Q_PROPERTY(QVariantMap defaultVersions READ defaultVersions NOTIFY defaultVersionsChanged)

public:
    explicit ProviderController(QObject *parent = nullptr);
    void startEventBus();

    QVariantList versions() const;
    bool busy() const;
    double progress() const;
    QString status() const;
    QString error() const;
    QString activeProvider() const;
    QString activeVersion() const;
    QVariantMap defaultVersions() const;

    Q_INVOKABLE void loadVersions(const QString &providerId, bool forceRefresh = false);
    Q_INVOKABLE void download(const QString &providerId, const QString &version,
                              bool allowUnverified = false);
    Q_INVOKABLE bool isDownloaded(const QString &providerId, const QString &version) const;
    Q_INVOKABLE void installDownloaded(const QString &providerId, const QString &version,
                                       bool makeDefault = false);
    Q_INVOKABLE void setDefaultVersion(const QString &providerId, const QString &version);
    Q_INVOKABLE void removeDownloaded(const QString &providerId, const QString &version);
    Q_INVOKABLE void cancel();

signals:
    void versionsChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();
    void errorChanged();
    void activeProviderChanged();
    void activeVersionChanged();
    void defaultVersionsChanged();
    void downloadFinished(const QString &providerId, const QString &version, const QString &path);
    void downloadRemoved(const QString &providerId, const QString &version);

private:
    void setBusy(bool busy);
    void setProgress(double progress);
    void setStatus(const QString &status);
    void setError(const QString &error);
    void fail(const QString &message);
    void fetchNodeChecksum();
    void verifyNodeDownload(const QByteArray &checksumDocument);
    void verifyDownloadedFile(const QByteArray &expectedHash);
    void verifyPythonDownload();
    void recordUnverifiedDownload();
    bool applyNodeIndex(const QByteArray &data);
    bool applyFlutterIndex(const QByteArray &data);
    bool applyJavaIndex(const QByteArray &data);
    bool applyPythonIndex(const QByteArray &data);
    bool applyPhpIndex(const QByteArray &data);
    bool applyGoIndex(const QByteArray &data);
    void fetchJavaIndexes();
    void fetchPhpIndexes();
    QString cachePath(const QString &providerId) const;
    QString downloadDirectory(const QString &providerId, const QString &version) const;
    QString downloadManifestPath(const QString &providerId, const QString &version) const;
    QString defaultsPath() const;
    QString installDirectory(const QString &providerId, const QString &version) const;
    QString shimDirectory() const;
    void loadDefaults();
    bool saveDefaults();
    void installAndActivateNode(const QString &version);
    bool activateNode(const QString &version);
    void installAndActivateFlutter(const QString &version);
    bool activateFlutter(const QString &version);
    void installAndActivateJava(const QString &version);
    bool activateJava(const QString &version);
    void installAndActivatePython(const QString &version);
    bool activatePython(const QString &version);
    void installAndActivatePhp(const QString &version);
    bool activatePhp(const QString &version);
    void installAndActivateGo(const QString &version);
    bool activateGo(const QString &version);
    bool deactivateProvider(const QString &providerId);
    bool ensureShimPath();
    bool writeCommandShim(const QString &name, const QString &target);
    QString nodeFileName(const QString &version) const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    QList<QPointer<QNetworkReply>> m_javaReplies;
    QFile m_downloadFile;
    QVariantList m_versions;
    bool m_busy = false;
    double m_progress = 0.0;
    QString m_status;
    QString m_error;
    QString m_providerId;
    QString m_version;
    QString m_downloadPath;
    qint64 m_resumeOffset = 0;
    QByteArray m_expectedHash;
    QString m_verificationMode;
    QVariantMap m_defaultVersions;
    QProcess m_installProcess;
    QString m_pendingInstallVersion;
    QString m_pendingStagingPath;
    bool m_makeDefaultAfterInstall = false;
    SvmEventBus *m_eventBus = nullptr;
    quint64 m_externalEventSerial = 0;
};
