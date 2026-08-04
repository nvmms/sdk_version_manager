#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVersionNumber>

#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <winioctl.h>
#endif

#include "../src/eventbus.h"
#include "../src/networkproxyconfig.h"
#include "../src/providercontroller.h"

namespace {
QTextStream out(stdout);
QTextStream err(stderr);
QTextStream input(stdin);
bool verboseOutput = false;

QString dataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString findProjectRoot(QString directory)
{
    QDir current(QDir::cleanPath(directory));
    while (true) {
        if (QFileInfo(current.filePath(QStringLiteral(".svmrc"))).isFile())
            return current.absolutePath();
        if (!current.cdUp())
            return {};
    }
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeObject(const QString &path, const QJsonObject &object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

bool writeProjectBinding(const QString &root, const QString &provider, const QString &version)
{
    const QString path = QDir(root).filePath(QStringLiteral(".svmrc"));
    QJsonObject config = readObject(path);
    QJsonObject sdks = config.value(QStringLiteral("sdks")).toObject();
    sdks.insert(provider, version);
    config.insert(QStringLiteral("schemaVersion"), 1);
    config.insert(QStringLiteral("sdks"), sdks);
    return writeObject(path, config);
}

bool writeProjectBindings(const QString &root, const QJsonObject &bindings)
{
    const QString path = QDir(root).filePath(QStringLiteral(".svmrc"));
    QJsonObject config = readObject(path);
    QJsonObject sdks = config.value(QStringLiteral("sdks")).toObject();
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it)
        sdks.insert(it.key(), it.value());
    config.insert(QStringLiteral("schemaVersion"), 1);
    config.insert(QStringLiteral("sdks"), sdks);
    return writeObject(path, config);
}

bool appendGitignoreEntry(const QString &path, const QByteArray &content)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        if (file.write(content) == content.size() && file.commit())
            return true;
        file.cancelWriting();
    }

    // On Windows, editors and file watchers can temporarily prevent QSaveFile from
    // replacing an existing .gitignore. Recheck before using an in-place append so
    // a concurrent writer cannot make us add the entry twice.
    QFile fallback(path);
    if (!fallback.open(QIODevice::ReadWrite))
        return false;
    QByteArray current = fallback.readAll();
    const QList<QByteArray> lines = current.replace("\r\n", "\n").split('\n');
    if (lines.contains(QByteArrayLiteral(".svm/")))
        return true;
    if (!fallback.seek(fallback.size()))
        return false;
    QByteArray suffix;
    if (!current.isEmpty() && !current.endsWith('\n'))
        suffix.append('\n');
    suffix.append(".svm/\n");
    return fallback.write(suffix) == suffix.size() && fallback.flush();
}

bool ensureProjectDirectoryIsIgnored(const QString &root)
{
    const QString path = QDir(root).filePath(QStringLiteral(".gitignore"));
    QFile existing(path);
    QByteArray content;
    if (existing.open(QIODevice::ReadOnly))
        content = existing.readAll();
    const QList<QByteArray> lines = QByteArray(content).replace("\r\n", "\n").split('\n');
    if (lines.contains(QByteArrayLiteral(".svm/")))
        return true;
    if (!content.isEmpty() && !content.endsWith('\n'))
        content.append('\n');
    content.append(".svm/\n");
    return appendGitignoreEntry(path, content);
}

QString projectConfigPath(const QString &root)
{
    return QDir(root).filePath(QStringLiteral(".svmrc"));
}

QString installedExecutable(const QString &provider, const QString &version)
{
    const QString root = dataRoot() + QStringLiteral("/installs/") + provider + u'/' + version;
    if (provider == QStringLiteral("node"))
        return root + QStringLiteral("/node.exe");
    if (provider == QStringLiteral("flutter"))
        return root + QStringLiteral("/bin/flutter.bat");
    if (provider == QStringLiteral("java"))
        return root + QStringLiteral("/bin/java.exe");
    if (provider == QStringLiteral("python"))
        return root + QStringLiteral("/python.exe");
    if (provider == QStringLiteral("php"))
        return root + QStringLiteral("/php.exe");
    if (provider == QStringLiteral("go"))
        return root + QStringLiteral("/bin/go.exe");
    if (provider == QStringLiteral("postgresql"))
        return root + QStringLiteral("/bin/psql.exe");
    if (provider == QStringLiteral("maven"))
        return root + QStringLiteral("/bin/mvn.cmd");
    return {};
}

QString installedVersionDirectory(const QString &provider, const QString &version)
{
    return QDir::cleanPath(dataRoot() + QStringLiteral("/installs/") + provider + u'/' + version);
}

struct ProviderEntryPoint
{
    QString provider;
    QString relativeExecutable;
    QStringList leadingArguments;
    bool requiresCommandInterpreter = false;
};

bool providerEntryPoint(const QString &command, ProviderEntryPoint *entryPoint)
{
    static const QHash<QString, ProviderEntryPoint> entries{
        {QStringLiteral("node"), {QStringLiteral("node"), QStringLiteral("node.exe"), {}}},
        {QStringLiteral("npm"), {QStringLiteral("node"), QStringLiteral("node.exe"),
                                  {QStringLiteral("node_modules/npm/bin/npm-cli.js")}}},
        {QStringLiteral("npx"), {QStringLiteral("node"), QStringLiteral("node.exe"),
                                  {QStringLiteral("node_modules/npm/bin/npx-cli.js")}}},
        {QStringLiteral("corepack"), {QStringLiteral("node"), QStringLiteral("node.exe"),
                                       {QStringLiteral("node_modules/corepack/dist/corepack.js")}}},
        {QStringLiteral("flutter"), {QStringLiteral("flutter"), QStringLiteral("bin/flutter.bat"), {}, true}},
        {QStringLiteral("dart"), {QStringLiteral("flutter"), QStringLiteral("bin/dart.bat"), {}, true}},
        {QStringLiteral("java"), {QStringLiteral("java"), QStringLiteral("bin/java.exe"), {}}},
        {QStringLiteral("javac"), {QStringLiteral("java"), QStringLiteral("bin/javac.exe"), {}}},
        {QStringLiteral("jar"), {QStringLiteral("java"), QStringLiteral("bin/jar.exe"), {}}},
        {QStringLiteral("javadoc"), {QStringLiteral("java"), QStringLiteral("bin/javadoc.exe"), {}}},
        {QStringLiteral("jshell"), {QStringLiteral("java"), QStringLiteral("bin/jshell.exe"), {}}},
        {QStringLiteral("keytool"), {QStringLiteral("java"), QStringLiteral("bin/keytool.exe"), {}}},
        {QStringLiteral("python"), {QStringLiteral("python"), QStringLiteral("python.exe"), {}}},
        {QStringLiteral("pip"), {QStringLiteral("python"), QStringLiteral("python.exe"),
                                  {QStringLiteral("-m"), QStringLiteral("pip")}}},
        {QStringLiteral("pip3"), {QStringLiteral("python"), QStringLiteral("python.exe"),
                                   {QStringLiteral("-m"), QStringLiteral("pip")}}},
        {QStringLiteral("php"), {QStringLiteral("php"), QStringLiteral("php.exe"), {}}},
        {QStringLiteral("go"), {QStringLiteral("go"), QStringLiteral("bin/go.exe"), {}}},
        {QStringLiteral("gofmt"), {QStringLiteral("go"), QStringLiteral("bin/gofmt.exe"), {}}},
        {QStringLiteral("postgresql"), {QStringLiteral("postgresql"), QStringLiteral("bin/psql.exe"), {}}},
        {QStringLiteral("psql"), {QStringLiteral("postgresql"), QStringLiteral("bin/psql.exe"), {}}},
        {QStringLiteral("postgres"), {QStringLiteral("postgresql"), QStringLiteral("bin/postgres.exe"), {}}},
        {QStringLiteral("pg_ctl"), {QStringLiteral("postgresql"), QStringLiteral("bin/pg_ctl.exe"), {}}},
        {QStringLiteral("pg_dump"), {QStringLiteral("postgresql"), QStringLiteral("bin/pg_dump.exe"), {}}},
        {QStringLiteral("pg_restore"), {QStringLiteral("postgresql"), QStringLiteral("bin/pg_restore.exe"), {}}},
        {QStringLiteral("createdb"), {QStringLiteral("postgresql"), QStringLiteral("bin/createdb.exe"), {}}},
        {QStringLiteral("dropdb"), {QStringLiteral("postgresql"), QStringLiteral("bin/dropdb.exe"), {}}},
        {QStringLiteral("createuser"), {QStringLiteral("postgresql"), QStringLiteral("bin/createuser.exe"), {}}},
        {QStringLiteral("dropuser"), {QStringLiteral("postgresql"), QStringLiteral("bin/dropuser.exe"), {}}},
        {QStringLiteral("maven"), {QStringLiteral("maven"), QStringLiteral("bin/mvn.cmd"), {}, true}},
        {QStringLiteral("mvn"), {QStringLiteral("maven"), QStringLiteral("bin/mvn.cmd"), {}, true}},
        {QStringLiteral("mvndebug"), {QStringLiteral("maven"), QStringLiteral("bin/mvnDebug.cmd"), {}, true}},
    };
    const auto found = entries.constFind(command);
    if (found == entries.cend())
        return false;
    *entryPoint = found.value();
    return true;
}

std::filesystem::path filesystemPath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

bool createDirectoryLink(const QString &target, const QString &link, QString *errorMessage)
{
#ifdef Q_OS_WIN
    const auto linkPath = reinterpret_cast<LPCWSTR>(link.utf16());
    const auto targetPath = reinterpret_cast<LPCWSTR>(target.utf16());
    // 0x2 requests unprivileged link creation when Windows Developer Mode is enabled.
    if (CreateSymbolicLinkW(linkPath, targetPath, SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        return true;
    }
    DWORD windowsError = GetLastError();
    if (windowsError == ERROR_INVALID_PARAMETER
        && CreateSymbolicLinkW(linkPath, targetPath, SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        return true;
    }
    windowsError = GetLastError();

    // Directory junctions do not require the symbolic-link privilege.
    if (windowsError == ERROR_PRIVILEGE_NOT_HELD) {
        QString substituteName = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
        if (substituteName.startsWith(QStringLiteral("\\\\")))
            substituteName = QStringLiteral("\\??\\UNC\\") + substituteName.mid(2);
        else
            substituteName.prepend(QStringLiteral("\\??\\"));
        const QString printName = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
        const USHORT substituteBytes = USHORT(substituteName.size() * sizeof(wchar_t));
        const USHORT printBytes = USHORT(printName.size() * sizeof(wchar_t));
        const USHORT pathBytes =
            USHORT(substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t));
        constexpr DWORD headerBytes = 8;
        constexpr DWORD mountPointHeaderBytes = 8;
        std::vector<BYTE> buffer(headerBytes + mountPointHeaderBytes + pathBytes, 0);
        auto writeWord = [&](DWORD offset, USHORT value) {
            memcpy(buffer.data() + offset, &value, sizeof(value));
        };
        const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        memcpy(buffer.data(), &tag, sizeof(tag));
        writeWord(4, USHORT(mountPointHeaderBytes + pathBytes));
        writeWord(8, 0);
        writeWord(10, substituteBytes);
        writeWord(12, USHORT(substituteBytes + sizeof(wchar_t)));
        writeWord(14, printBytes);
        memcpy(buffer.data() + headerBytes + mountPointHeaderBytes,
               substituteName.utf16(), substituteBytes);
        memcpy(buffer.data() + headerBytes + mountPointHeaderBytes
                   + substituteBytes + sizeof(wchar_t),
               printName.utf16(), printBytes);

        if (CreateDirectoryW(linkPath, nullptr)) {
            HANDLE directory =
                CreateFileW(linkPath, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (directory != INVALID_HANDLE_VALUE) {
                DWORD returnedBytes = 0;
                const BOOL created =
                    DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                    DWORD(buffer.size()), nullptr, 0, &returnedBytes, nullptr);
                const DWORD junctionError = created ? ERROR_SUCCESS : GetLastError();
                CloseHandle(directory);
                if (created)
                    return true;
                RemoveDirectoryW(linkPath);
                windowsError = junctionError;
            } else {
                windowsError = GetLastError();
                RemoveDirectoryW(linkPath);
            }
        } else {
            windowsError = GetLastError();
        }
    }
    if (errorMessage) {
        *errorMessage =
            QStringLiteral("Windows could not create a directory link or junction (error %1).")
                .arg(windowsError);
    }
    return false;
#else
    std::error_code error;
    std::filesystem::create_directory_symlink(filesystemPath(target), filesystemPath(link), error);
    if (!error)
        return true;
    if (errorMessage)
        *errorMessage = QString::fromLocal8Bit(error.message());
    return false;
#endif
}

bool syncProjectSdkMapping(const QString &root, const QString &provider, const QString &version,
                           QString *errorMessage)
{
    const QString target = installedVersionDirectory(provider, version);
    const QString mappingsRoot = QDir(root).filePath(QStringLiteral(".svm/sdks"));
    const QString link = QDir(mappingsRoot).filePath(provider);
    if (!QFileInfo(target).isDir()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Installed SDK directory does not exist: %1").arg(target);
        return false;
    }
    if (!QDir().mkpath(mappingsRoot)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to create %1").arg(mappingsRoot);
        return false;
    }

    const std::filesystem::path linkPath = filesystemPath(link);
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(linkPath, error);
    if (!error && std::filesystem::exists(status)) {
        bool isDirectoryLink = std::filesystem::is_symlink(status);
#ifdef Q_OS_WIN
        const DWORD attributes =
            GetFileAttributesW(reinterpret_cast<LPCWSTR>(link.utf16()));
        isDirectoryLink =
            isDirectoryLink
            || (attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_DIRECTORY)
                && (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
#endif
        if (!isDirectoryLink) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Refusing to replace non-link path: %1").arg(link);
            }
            return false;
        }
        std::filesystem::remove(linkPath, error);
        if (error) {
            if (errorMessage)
                *errorMessage = QString::fromLocal8Bit(error.message());
            return false;
        }
    } else if (error && error != std::errc::no_such_file_or_directory) {
        if (errorMessage)
            *errorMessage = QString::fromLocal8Bit(error.message());
        return false;
    }

    return createDirectoryLink(target, link, errorMessage);
}

QString globalDefaultVersion(const QString &provider)
{
    return readObject(dataRoot() + QStringLiteral("/settings/default-versions.json"))
        .value(QStringLiteral("providers")).toObject().value(provider).toString();
}

QString latestInstalledVersion(const QString &provider)
{
    QDir directory(dataRoot() + QStringLiteral("/installs/") + provider);
    QString best;
    QVersionNumber bestNumber;
    for (const QString &version : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QFileInfo(installedExecutable(provider, version)).isFile())
            continue;
        qsizetype suffixIndex = 0;
        const QVersionNumber number = QVersionNumber::fromString(version, &suffixIndex);
        if (number.isNull())
            continue;
        const bool preferNts =
            provider == QStringLiteral("php")
            && QVersionNumber::compare(number, bestNumber) == 0
            && version.endsWith(QStringLiteral("-nts"))
            && !best.endsWith(QStringLiteral("-nts"));
        const bool phpVariantTie =
            provider == QStringLiteral("php")
            && QVersionNumber::compare(number, bestNumber) == 0;
        if (best.isEmpty() || QVersionNumber::compare(number, bestNumber) > 0
            || preferNts
            || (QVersionNumber::compare(number, bestNumber) == 0
                && !phpVariantTie && version > best)) {
            best = version;
            bestNumber = number;
        }
    }
    return best;
}

void waitUntilIdle(ProviderController &controller)
{
    if (!controller.busy())
        return;
    QEventLoop loop;
    QObject::connect(&controller, &ProviderController::busyChanged, &loop, [&] {
        if (!controller.busy())
            loop.quit();
    });
    loop.exec();
}

bool downloadAndInstall(const QString &provider, const QString &version,
                        bool allowUnverifiedArchive = false, bool verbose = false)
{
    ProviderController controller;
    QObject::connect(&controller, &ProviderController::diagnostic, &controller,
                     [](const QString &message) {
        out << '\r' << QString(100, u' ') << '\r' << Qt::flush;
        err << "[verbose] " << message << Qt::endl;
    });
    controller.setVerbose(verbose);
    const auto publish = [&](const QString &type, double progress = -1.0) {
        QJsonObject event{
            {QStringLiteral("type"), type},
            {QStringLiteral("provider"), provider},
            {QStringLiteral("version"), version}
        };
        if (progress >= 0.0)
            event.insert(QStringLiteral("progress"), progress);
        publishSvmEvent(event);
    };
    QTimer heartbeat;
    heartbeat.setInterval(1000);
    QObject::connect(&heartbeat, &QTimer::timeout, &controller, [&] {
        publish(QStringLiteral("heartbeat"), controller.progress());
    });
    heartbeat.start();

    out << "Reading the " << provider << " version index..." << Qt::endl;
    controller.loadVersions(provider, false);
    waitUntilIdle(controller);
    if (!controller.error().isEmpty() || controller.versions().isEmpty()) {
        publish(QStringLiteral("error"));
        err << "Failed to load the " << provider << " version index." << Qt::endl;
        if (verbose && !controller.error().isEmpty())
            err << "[verbose] " << controller.error() << Qt::endl;
        return false;
    }

    bool unverifiedArchive = false;
    bool versionFound = false;
    QStringList matchingVersions;
    for (const QVariant &entry : controller.versions()) {
        const QVariantMap item = entry.toMap();
        const QString availableVersion = item.value(QStringLiteral("version")).toString();
        if (availableVersion == version) {
            versionFound = true;
            unverifiedArchive = item.value(QStringLiteral("unverified")).toBool();
        } else if (availableVersion.contains(version, Qt::CaseInsensitive)) {
            matchingVersions.append(availableVersion);
        }
    }
    if (!versionFound) {
        publish(QStringLiteral("error"));
        err << provider << " version '" << version << "' was not found in the official index."
            << Qt::endl;
        if (!matchingVersions.isEmpty()) {
            err << "Matching versions:" << Qt::endl;
            const qsizetype limit = qMin<qsizetype>(matchingVersions.size(), 10);
            for (qsizetype i = 0; i < limit; ++i)
                err << "  " << matchingVersions[i] << Qt::endl;
            if (matchingVersions.size() > limit)
                err << "  ... and " << matchingVersions.size() - limit << " more" << Qt::endl;
        } else {
            err << "Run `svm list " << provider << "` to see available versions." << Qt::endl;
        }
        return false;
    }
    if (unverifiedArchive && !allowUnverifiedArchive) {
#ifdef Q_OS_WIN
        const bool interactive =
            GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
        const bool interactive = false;
#endif
        if (!interactive) {
            err << provider << ' ' << version
                << " has no official checksum or signature. Re-run with "
                   "`--allow-unverified-archive` to accept this risk."
                << Qt::endl;
            return false;
        }
        out << "Warning: " << provider << ' ' << version
            << " has no checksum that SVM can verify automatically."
            << Qt::endl
            << "SVM can only use HTTPS and record a local integrity hash. Continue? [y/N] "
            << Qt::flush;
        const QString answer = input.readLine().trimmed().toLower();
        if (answer != QStringLiteral("y") && answer != QStringLiteral("yes"))
            return false;
        allowUnverifiedArchive = true;
    }

    const QString displayName = provider == QStringLiteral("node")
        ? QStringLiteral("Node.js")
        : provider == QStringLiteral("flutter") ? QStringLiteral("Flutter")
        : provider == QStringLiteral("java") ? QStringLiteral("Java")
        : provider == QStringLiteral("python") ? QStringLiteral("Python")
        : provider == QStringLiteral("php") ? QStringLiteral("PHP")
        : provider == QStringLiteral("go") ? QStringLiteral("Go")
        : provider == QStringLiteral("maven") ? QStringLiteral("Apache Maven")
                                               : QStringLiteral("PostgreSQL");
    int lastPercent = -1;
    const auto renderProgress = [&] {
        const int percent = qBound(0, qRound(controller.progress() * 100.0), 100);
        if (percent == lastPercent)
            return;
        lastPercent = percent;
        constexpr int width = 30;
        const int completed = percent * width / 100;
        QString bar(completed, u'=');
        if (completed < width)
            bar += u'>';
        bar += QString(qMax(0, width - completed - 1), u' ');
        out << '\r' << "Downloading " << displayName << ' ' << version
            << "  [" << bar << "] " << QString::number(percent).rightJustified(3) << '%'
            << Qt::flush;
        publish(QStringLiteral("download-progress"), controller.progress());
    };
    QObject::connect(&controller, &ProviderController::progressChanged,
                     &controller, renderProgress);
    publish(QStringLiteral("download-start"), 0.0);
    renderProgress();
    controller.download(provider, version, allowUnverifiedArchive);
    waitUntilIdle(controller);
    out << '\r';
    if (!controller.error().isEmpty() || !controller.isDownloaded(provider, version)) {
        publish(QStringLiteral("error"));
        err << "Failed to download " << displayName << ' ' << version << '.'
            << QString(45, u' ') << Qt::endl;
        if (verbose && !controller.error().isEmpty())
            err << "[verbose] " << controller.error() << Qt::endl;
        return false;
    }
    if (lastPercent < 100) {
        lastPercent = -1;
        renderProgress();
    }
    out << Qt::endl;

    out << "Installing " << displayName << ' ' << version << "..." << Qt::flush;
    publish(QStringLiteral("install-start"), 1.0);
    controller.installDownloaded(provider, version, false);
    waitUntilIdle(controller);
    if (!controller.error().isEmpty()
        || !QFileInfo(installedExecutable(provider, version)).isFile()) {
        publish(QStringLiteral("error"));
        err << Qt::endl << "Failed to install " << displayName << ' ' << version << '.'
            << Qt::endl;
        return false;
    }
    out << " done." << Qt::endl;
    publish(QStringLiteral("done"), 1.0);
    return true;
}

int javaMajorVersion(const QString &version)
{
    const QString first = version.section(u'.', 0, 0);
    if (first == QStringLiteral("1"))
        return version.section(u'.', 1, 1).toInt();
    return first.toInt();
}

int minimumJavaForMaven(const QString &mavenVersion)
{
    return mavenVersion.startsWith(QStringLiteral("4.")) ? 17 : 8;
}

bool isInstalledJavaCompatible(const QString &version, int minimumMajor)
{
    return javaMajorVersion(version) >= minimumMajor
        && QFileInfo(installedExecutable(QStringLiteral("java"), version)).isFile();
}

QString mavenRuntimeJava(const QString &mavenVersion)
{
    return readObject(dataRoot() + QStringLiteral("/settings/runtime-dependencies.json"))
        .value(QStringLiteral("maven")).toObject().value(mavenVersion).toObject()
        .value(QStringLiteral("java")).toString();
}

bool saveMavenRuntimeJava(const QString &mavenVersion, const QString &javaVersion)
{
    const QString path = dataRoot() + QStringLiteral("/settings/runtime-dependencies.json");
    QJsonObject root = readObject(path);
    QJsonObject maven = root.value(QStringLiteral("maven")).toObject();
    maven.insert(mavenVersion, QJsonObject{{QStringLiteral("java"), javaVersion},
                                           {QStringLiteral("automatic"), true}});
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("maven"), maven);
    return writeObject(path, root);
}

QString ensureMavenJava(const QString &mavenVersion)
{
    const int minimumMajor = minimumJavaForMaven(mavenVersion);
    const QString projectRoot = findProjectRoot(QDir::currentPath());
    if (!projectRoot.isEmpty()) {
        const QString bound = readObject(projectConfigPath(projectRoot))
                                  .value(QStringLiteral("sdks")).toObject()
                                  .value(QStringLiteral("java")).toString();
        if (!bound.isEmpty()) {
            if (!isInstalledJavaCompatible(bound, minimumMajor)) {
                err << "Project Java " << bound << " is not an installed compatible runtime for Maven "
                    << mavenVersion << " (requires Java " << minimumMajor << "+)." << Qt::endl;
                return {};
            }
            return bound;
        }
    }
    const QString global = globalDefaultVersion(QStringLiteral("java"));
    if (isInstalledJavaCompatible(global, minimumMajor))
        return global;

    QString selected;
    QVersionNumber selectedNumber;
    QDir installs(dataRoot() + QStringLiteral("/installs/java"));
    for (const QString &candidate : installs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!isInstalledJavaCompatible(candidate, minimumMajor))
            continue;
        const QVersionNumber number = QVersionNumber::fromString(candidate);
        const bool candidateIsLts = QList<int>{8, 11, 17, 21}.contains(javaMajorVersion(candidate));
        const bool selectedIsLts = QList<int>{8, 11, 17, 21}.contains(javaMajorVersion(selected));
        if (selected.isEmpty() || (candidateIsLts && !selectedIsLts)
            || (candidateIsLts == selectedIsLts
                && QVersionNumber::compare(number, selectedNumber) > 0)) {
            selected = candidate;
            selectedNumber = number;
        }
    }
    if (!selected.isEmpty())
        return selected;

    ProviderController controller;
    controller.setVerbose(verboseOutput);
    out << "No compatible managed Java is installed; resolving a recommended Java LTS..."
        << Qt::endl;
    controller.loadVersions(QStringLiteral("java"), false);
    waitUntilIdle(controller);
    QVersionNumber bestNumber;
    for (const QVariant &value : controller.versions()) {
        const QString candidate = value.toMap().value(QStringLiteral("version")).toString();
        const int major = javaMajorVersion(candidate);
        if (major != 21 || major < minimumMajor)
            continue;
        const QVersionNumber number = QVersionNumber::fromString(candidate);
        if (selected.isEmpty() || QVersionNumber::compare(number, bestNumber) > 0) {
            selected = candidate;
            bestNumber = number;
        }
    }
    if (selected.isEmpty()) {
        err << "No recommended Java 21 LTS release is available for Maven "
            << mavenVersion << '.' << Qt::endl;
        return {};
    }
    out << "Maven " << mavenVersion << " requires Java " << minimumMajor
        << "+; installing Java " << selected << " automatically." << Qt::endl;
    if (!downloadAndInstall(QStringLiteral("java"), selected, false, verboseOutput))
        return {};
    return selected;
}

QString resolveVersion(const QString &provider, const QString &workingDirectory, QString *source)
{
    const QString projectRoot = findProjectRoot(workingDirectory);
    if (!projectRoot.isEmpty()) {
        const QJsonObject config = readObject(projectConfigPath(projectRoot));
        const QString version =
            config.value(QStringLiteral("sdks")).toObject().value(provider).toString();
        if (!version.isEmpty()) {
            if (source)
                *source = projectConfigPath(projectRoot);
            return version;
        }
    }

    const QString defaultsPath = dataRoot() + QStringLiteral("/settings/default-versions.json");
    const QString version = readObject(defaultsPath)
                                .value(QStringLiteral("providers")).toObject()
                                .value(provider).toString();
    if (!version.isEmpty() && source)
        *source = QStringLiteral("global");
    return version;
}

bool validToken(const QString &value)
{
    if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral(".."))
        return false;
    for (const QChar character : value) {
        if (!character.isLetterOrNumber() && character != u'.' && character != u'-'
            && character != u'_')
            return false;
    }
    return true;
}

int commandActive();

QString javaRuntimeName(const QString &version)
{
    const QStringList parts = version.split(u'.');
    QString major = parts.value(0);
    if (major == QStringLiteral("1") && parts.size() > 1)
        major = parts[1];
    return major.isEmpty() ? QStringLiteral("JavaSE") : QStringLiteral("JavaSE-") + major;
}

QJsonObject vscodeSettingsTemplate(const QJsonObject &sdks)
{
    QJsonObject settings;
    if (sdks.contains(QStringLiteral("flutter"))) {
        settings.insert(QStringLiteral("dart.flutterSdkPath"),
                        QStringLiteral(".svm/sdks/flutter"));
    }
    if (sdks.contains(QStringLiteral("python"))) {
        settings.insert(QStringLiteral("python.defaultInterpreterPath"),
                        QStringLiteral(".svm/sdks/python"));
    }
    if (sdks.contains(QStringLiteral("java"))) {
        settings.insert(
            QStringLiteral("java.configuration.runtimes"),
            QJsonArray{QJsonObject{
                {QStringLiteral("name"),
                 javaRuntimeName(sdks.value(QStringLiteral("java")).toString())},
                {QStringLiteral("path"), QStringLiteral(".svm/sdks/java")},
                {QStringLiteral("default"), true}
            }});
    }
    return settings;
}

void printIdeConfigurationHints(const QString &projectRoot, const QJsonObject &sdks)
{
    const QJsonObject vscodeSettings = vscodeSettingsTemplate(sdks);
    out << Qt::endl
        << "IDE configuration (no IDE files were written):" << Qt::endl;
    if (!vscodeSettings.isEmpty()) {
        out << Qt::endl << "VS Code" << Qt::endl
            << "Add or merge into "
            << QDir(projectRoot).filePath(QStringLiteral(".vscode/settings.json"))
            << ':' << Qt::endl
            << QJsonDocument(vscodeSettings).toJson(QJsonDocument::Indented)
            << "Apply explicitly: svm ide vscode" << Qt::endl;
    }
    if (sdks.contains(QStringLiteral("node"))) {
        out << "For a VS Code Node.js launch configuration, set runtimeExecutable to:"
            << Qt::endl;
#ifdef Q_OS_WIN
        out << "  ${workspaceFolder}/.svm/sdks/node/node.exe" << Qt::endl;
#else
        out << "  ${workspaceFolder}/.svm/sdks/node/bin/node" << Qt::endl;
#endif
    }
    if (sdks.contains(QStringLiteral("flutter"))
        || sdks.contains(QStringLiteral("java"))
        || sdks.contains(QStringLiteral("python"))
        || sdks.contains(QStringLiteral("node"))) {
        const auto sdkPath = [&](const QString &provider) {
            return QDir::toNativeSeparators(
                QDir(projectRoot).filePath(QStringLiteral(".svm/sdks/") + provider));
        };
        out << Qt::endl << "IntelliJ IDEA" << Qt::endl;
        if (sdks.contains(QStringLiteral("flutter"))) {
            out << "Settings > Languages & Frameworks > Flutter > Flutter SDK path:"
                << Qt::endl << "  " << sdkPath(QStringLiteral("flutter")) << Qt::endl;
        }
        if (sdks.contains(QStringLiteral("java"))) {
            out << "Project Structure > SDKs > JDK home:" << Qt::endl
                << "  " << sdkPath(QStringLiteral("java")) << Qt::endl;
        }
        if (sdks.contains(QStringLiteral("python"))) {
            out << "Settings > Python Interpreter > Add local interpreter:"
                << Qt::endl << "  " << sdkPath(QStringLiteral("python")) << Qt::endl;
        }
        if (sdks.contains(QStringLiteral("node"))) {
            out << "Settings > Languages & Frameworks > Node.js > Node interpreter:"
                << Qt::endl;
#ifdef Q_OS_WIN
            out << "  " << sdkPath(QStringLiteral("node")) << "\\node.exe" << Qt::endl;
#else
            out << "  " << sdkPath(QStringLiteral("node")) << "/bin/node" << Qt::endl;
#endif
        }
        out << "Show again: svm ide idea" << Qt::endl
            << Qt::endl << "Android Studio" << Qt::endl;
        if (sdks.contains(QStringLiteral("flutter"))) {
            out << "Settings > Languages & Frameworks > Flutter > Flutter SDK path:"
                << Qt::endl << "  " << sdkPath(QStringLiteral("flutter")) << Qt::endl;
        }
        if (sdks.contains(QStringLiteral("java"))) {
            out << "Settings > Build Tools > Gradle > Gradle JDK:"
                << Qt::endl << "  " << sdkPath(QStringLiteral("java")) << Qt::endl;
        }
        if (sdks.contains(QStringLiteral("node"))) {
            out << "Node.js requires the JavaScript/Node.js plugin; set its Node interpreter to:"
                << Qt::endl;
#ifdef Q_OS_WIN
            out << "  " << sdkPath(QStringLiteral("node")) << "\\node.exe" << Qt::endl;
#else
            out << "  " << sdkPath(QStringLiteral("node")) << "/bin/node" << Qt::endl;
#endif
        }
        out << "Show again: svm ide android-studio" << Qt::endl;
    }
}

bool configureVSCode(const QString &projectRoot, const QJsonObject &sdks,
                     QString *settingsPathResult, bool *changedResult,
                     QString *errorMessage)
{
    if (changedResult)
        *changedResult = false;
    const bool hasConfigurableBinding =
        sdks.contains(QStringLiteral("flutter"))
        || sdks.contains(QStringLiteral("python"))
        || sdks.contains(QStringLiteral("java"));
    const QString settingsPath =
        QDir(projectRoot).filePath(QStringLiteral(".vscode/settings.json"));

    QJsonObject settings;
    if (QFileInfo(settingsPath).exists()) {
        QFile settingsFile(settingsPath);
        if (!settingsFile.open(QIODevice::ReadOnly)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Failed to read %1").arg(settingsPath);
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(settingsFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Cannot safely update %1: %2. Add "
                                   "\"dart.flutterSdkPath\": \".svm/sdks/flutter\" manually.")
                        .arg(settingsPath, parseError.errorString());
            }
            return false;
        }
        settings = document.object();
    }
    const QJsonObject originalSettings = settings;

    if (sdks.contains(QStringLiteral("flutter"))) {
        settings.insert(QStringLiteral("dart.flutterSdkPath"),
                        QStringLiteral(".svm/sdks/flutter"));
    } else if (settings.value(QStringLiteral("dart.flutterSdkPath")).toString()
               == QStringLiteral(".svm/sdks/flutter")) {
        settings.remove(QStringLiteral("dart.flutterSdkPath"));
    }
    if (sdks.contains(QStringLiteral("python"))) {
        settings.insert(QStringLiteral("python.defaultInterpreterPath"),
                        QStringLiteral(".svm/sdks/python"));
    } else if (settings.value(QStringLiteral("python.defaultInterpreterPath")).toString()
               == QStringLiteral(".svm/sdks/python")) {
        settings.remove(QStringLiteral("python.defaultInterpreterPath"));
    }
    const QString javaPath = QStringLiteral(".svm/sdks/java");
    const QJsonArray runtimes = settings.value(QStringLiteral("java.configuration.runtimes"))
                                    .toArray();
    QJsonArray merged;
    for (const QJsonValue &value : runtimes) {
        if (value.toObject().value(QStringLiteral("path")).toString() != javaPath)
            merged.append(value);
    }
    if (sdks.contains(QStringLiteral("java"))) {
        merged.append(QJsonObject{
            {QStringLiteral("name"),
             javaRuntimeName(sdks.value(QStringLiteral("java")).toString())},
            {QStringLiteral("path"), javaPath},
            {QStringLiteral("default"), true}
        });
        settings.insert(QStringLiteral("java.configuration.runtimes"), merged);
    } else if (merged.size() != runtimes.size()) {
        if (merged.isEmpty())
            settings.remove(QStringLiteral("java.configuration.runtimes"));
        else
            settings.insert(QStringLiteral("java.configuration.runtimes"), merged);
    }
    if (settings == originalSettings) {
        if (settingsPathResult)
            *settingsPathResult = hasConfigurableBinding ? settingsPath : QString();
        return true;
    }
    if (!writeObject(settingsPath, settings)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to update %1").arg(settingsPath);
        return false;
    }
    if (changedResult)
        *changedResult = true;
    if (settingsPathResult)
        *settingsPathResult = settingsPath;
    return true;
}

int commandInit(const QStringList &arguments)
{
    Q_UNUSED(arguments)
    const QString root = QDir::currentPath();
    const QString svmDir = QDir(root).filePath(QStringLiteral(".svm"));
    const QString configPath = QDir(root).filePath(QStringLiteral(".svmrc"));
    if (QFileInfo(configPath).exists()) {
        err << "SVM project already initialized: " << configPath << Qt::endl;
        return 2;
    }

    QJsonObject config;
    config.insert(QStringLiteral("schemaVersion"), 1);
    config.insert(QStringLiteral("sdks"), QJsonObject{});
    if (!QDir().mkpath(svmDir) || !writeObject(configPath, config)) {
        err << "Failed to create " << configPath << Qt::endl;
        return 1;
    }

    const QString ignorePath = QDir(root).filePath(QStringLiteral(".gitignore"));
    if (!ensureProjectDirectoryIsIgnored(root)) {
        err << "Initialized config, but failed to update " << ignorePath << Qt::endl;
        return 1;
    }
    out << "Initialized SVM project: " << configPath << Qt::endl;
    return 0;
}

int commandUse(const QStringList &rawArguments)
{
    QStringList arguments = rawArguments;
    const bool allowUnverifiedArchive =
        arguments.removeAll(QStringLiteral("--allow-unverified-archive")) > 0;
    if (arguments.isEmpty())
        return commandActive();
    if (arguments.size() == 1 && validToken(arguments[0])) {
        const QString provider = arguments[0].toLower();
        if (installedExecutable(provider, QString()).isEmpty()) {
            err << "Unsupported provider: " << provider << Qt::endl;
            return 2;
        }
        QString source;
        QString version;
        const QString projectRoot = findProjectRoot(QDir::currentPath());
        if (!projectRoot.isEmpty()) {
            version = readObject(projectConfigPath(projectRoot))
                          .value(QStringLiteral("sdks")).toObject()
                          .value(provider).toString();
            if (!version.isEmpty())
                source = projectConfigPath(projectRoot);
        }
        if (version.isEmpty()) {
            version = globalDefaultVersion(provider);
            if (!version.isEmpty())
                source = QStringLiteral("global");
        }
        if (version.isEmpty()) {
            version = latestInstalledVersion(provider);
            source = QStringLiteral("latest installed");
        }
        if (version.isEmpty()) {
            out << "No installed " << provider << " version. Enter a version to download: "
                << Qt::flush;
            version = input.readLine().trimmed();
            if (!validToken(version)) {
                err << "A valid version is required." << Qt::endl;
                return 2;
            }
            source = QStringLiteral("requested");
        }
        out << provider << ": " << version << " (" << source << ')' << Qt::endl;
        QStringList resolvedArguments{provider, version};
        if (allowUnverifiedArchive)
            resolvedArguments.append(QStringLiteral("--allow-unverified-archive"));
        return commandUse(resolvedArguments);
    }
    if (arguments.size() != 2 || !validToken(arguments[0]) || !validToken(arguments[1])) {
        err << "Usage: svm use [provider [version]]" << Qt::endl;
        return 2;
    }
    QString root = findProjectRoot(QDir::currentPath());
    const bool needsInitialization = root.isEmpty();
    if (needsInitialization)
        root = QDir::currentPath();

    const QString provider = arguments[0].toLower();
    const QString version = arguments[1];
    QString dependencyJava;
    if (provider == QStringLiteral("maven")) {
        dependencyJava = ensureMavenJava(version);
        if (dependencyJava.isEmpty())
            return 3;
    }
    const QString executable = installedExecutable(provider, version);
    if (executable.isEmpty()) {
        err << "Unsupported provider: " << provider << Qt::endl;
        return 2;
    }
    if (!QFileInfo(executable).isFile()) {
        if (!downloadAndInstall(provider, version, allowUnverifiedArchive, verboseOutput))
            return 3;
    }

    if (needsInitialization) {
        const int initResult = commandInit({});
        if (initResult != 0)
            return initResult;
    } else if (!ensureProjectDirectoryIsIgnored(root)) {
        err << "Failed to update " << QDir(root).filePath(QStringLiteral(".gitignore"))
            << Qt::endl;
        return 1;
    }

    QJsonObject bindings{{provider, version}};
    if (!dependencyJava.isEmpty())
        bindings.insert(QStringLiteral("java"), dependencyJava);
    if (!writeProjectBindings(root, bindings)) {
        err << "Failed to update " << projectConfigPath(root) << Qt::endl;
        return 1;
    }
    QString mappingError;
    if (!syncProjectSdkMapping(root, provider, version, &mappingError)) {
        err << "Updated " << projectConfigPath(root)
            << ", but failed to create the project SDK mapping: " << mappingError << Qt::endl;
        err << "Expected mapping: "
            << QDir(root).filePath(QStringLiteral(".svm/sdks/") + provider) << " -> "
            << installedVersionDirectory(provider, version) << Qt::endl;
        return 1;
    }
    if (!dependencyJava.isEmpty()
        && !syncProjectSdkMapping(root, QStringLiteral("java"), dependencyJava, &mappingError)) {
        err << "Updated " << projectConfigPath(root)
            << ", but failed to create the Java dependency mapping: " << mappingError << Qt::endl;
        return 1;
    }
    if (!dependencyJava.isEmpty()
        && !saveMavenRuntimeJava(version, dependencyJava)) {
        err << "Failed to record the Maven runtime dependency." << Qt::endl;
        return 1;
    }
    const QJsonObject sdks = readObject(projectConfigPath(root))
                                  .value(QStringLiteral("sdks")).toObject();
    out << provider << ' ' << version << " is now active for " << root << Qt::endl;
    if (!dependencyJava.isEmpty())
        out << "  runtime dependency: Java " << dependencyJava << Qt::endl;
    printIdeConfigurationHints(root, sdks);
    return 0;
}

int commandActive()
{
    const QString projectRoot = findProjectRoot(QDir::currentPath());
    bool mappingsHealthy = true;
    if (!projectRoot.isEmpty()) {
        const QJsonObject sdks = readObject(projectConfigPath(projectRoot))
                                      .value(QStringLiteral("sdks")).toObject();
        out << "Project: " << projectRoot << Qt::endl;
        if (sdks.isEmpty())
            out << "  No project SDK bindings." << Qt::endl;
        for (auto it = sdks.constBegin(); it != sdks.constEnd(); ++it) {
            const QString provider = it.key();
            const QString version = it.value().toString();
            out << "  " << provider << ": " << version << " (project)" << Qt::endl;
            if (!validToken(provider) || !validToken(version)
                || installedExecutable(provider, version).isEmpty()) {
                mappingsHealthy = false;
                err << "  Cannot synchronize invalid or unsupported binding: "
                    << provider << ' ' << version << Qt::endl;
                continue;
            }
            QString mappingError;
            if (!syncProjectSdkMapping(projectRoot, provider, version, &mappingError)) {
                mappingsHealthy = false;
                err << "  Failed to synchronize .svm/sdks/" << provider << ": "
                    << mappingError << Qt::endl;
            }
        }
    } else {
        out << "Project: none" << Qt::endl;
    }

    const QJsonObject defaults =
        readObject(dataRoot() + QStringLiteral("/settings/default-versions.json"))
            .value(QStringLiteral("providers")).toObject();
    out << "Global defaults:" << Qt::endl;
    if (defaults.isEmpty())
        out << "  none" << Qt::endl;
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        out << "  " << it.key() << ": " << it.value().toString() << Qt::endl;
    if (!projectRoot.isEmpty()) {
        const QJsonObject sdks = readObject(projectConfigPath(projectRoot))
                                      .value(QStringLiteral("sdks")).toObject();
        printIdeConfigurationHints(projectRoot, sdks);
    }
    return mappingsHealthy ? 0 : 1;
}

int commandIde(const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        out << "IDE integrations:" << Qt::endl
            << "  vscode         configure .vscode/settings.json" << Qt::endl
            << "  idea           show IntelliJ IDEA setup instructions" << Qt::endl
            << "  android-studio show Android Studio setup instructions" << Qt::endl;
        return 0;
    }
    if (arguments.size() != 1) {
        err << "Usage: svm ide [vscode|idea|android-studio]" << Qt::endl;
        return 2;
    }

    const QString projectRoot = findProjectRoot(QDir::currentPath());
    if (projectRoot.isEmpty()) {
        err << "No SVM project found. Run `svm init` first." << Qt::endl;
        return 3;
    }
    const QJsonObject sdks = readObject(projectConfigPath(projectRoot))
                                  .value(QStringLiteral("sdks")).toObject();
    if (sdks.isEmpty()) {
        err << "This project has no SDK bindings. Run `svm use <provider> <version>` first."
            << Qt::endl;
        return 3;
    }

    for (auto it = sdks.constBegin(); it != sdks.constEnd(); ++it) {
        if (installedExecutable(it.key(), it.value().toString()).isEmpty())
            continue;
        QString mappingError;
        if (!syncProjectSdkMapping(projectRoot, it.key(), it.value().toString(),
                                   &mappingError)) {
            err << "Failed to synchronize .svm/sdks/" << it.key() << ": "
                << mappingError << Qt::endl;
            return 1;
        }
    }

    const QString ide = arguments[0].toLower();
    if (ide == QStringLiteral("vscode") || ide == QStringLiteral("code")) {
        QString settingsPath;
        bool settingsChanged = false;
        QString ideError;
        if (!configureVSCode(projectRoot, sdks, &settingsPath, &settingsChanged,
                             &ideError)) {
            err << ideError << Qt::endl;
            return 1;
        }
        if (settingsPath.isEmpty()) {
            out << "No provider-specific VS Code workspace setting is required. "
                   "Run Node.js commands through `svm node ...`."
                << Qt::endl;
            return 0;
        }
        out << "VS Code: " << (settingsChanged ? "updated " : "already configured ")
            << settingsPath << Qt::endl
            << "  Applied settings for project SDK bindings." << Qt::endl;
        return 0;
    }

    if (ide == QStringLiteral("idea") || ide == QStringLiteral("intellij")
        || ide == QStringLiteral("android-studio")
        || ide == QStringLiteral("androidstudio")) {
        const QString displayName =
            (ide == QStringLiteral("android-studio")
             || ide == QStringLiteral("androidstudio"))
            ? QStringLiteral("Android Studio") : QStringLiteral("IntelliJ IDEA");
        out << displayName
            << " does not expose a stable project file for all SDK settings."
            << Qt::endl;
        for (auto it = sdks.constBegin(); it != sdks.constEnd(); ++it) {
            out << "  " << it.key() << ": "
                << QDir::toNativeSeparators(
                       QDir(projectRoot).filePath(QStringLiteral(".svm/sdks/") + it.key()))
                << Qt::endl;
        }
        return 0;
    }

    err << "Unsupported IDE: " << ide
        << ". Use vscode, idea, or android-studio." << Qt::endl;
    return 2;
}

QVariantList cachedVersions(const QString &provider)
{
    const QString path =
        dataRoot() + QStringLiteral("/cache/") + provider + QStringLiteral("/versions.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    const QJsonDocument document = QJsonDocument::fromJson(data);
    QVariantList result;

    if (provider == QStringLiteral("node") && document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            const QJsonObject release = value.toObject();
            bool windowsZip = false;
            for (const QJsonValue &fileValue : release.value(QStringLiteral("files")).toArray()) {
                if (fileValue.toString() == QStringLiteral("win-x64-zip")) {
                    windowsZip = true;
                    break;
                }
            }
            if (!windowsZip)
                continue;
            const QJsonValue lts = release.value(QStringLiteral("lts"));
            QVariantMap item;
            QString version = release.value(QStringLiteral("version")).toString();
            if (version.startsWith(u'v'))
                version.remove(0, 1);
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"),
                        lts.isString() ? QStringLiteral("LTS · ") + lts.toString()
                                       : QStringLiteral("current"));
            item.insert(QStringLiteral("date"), release.value(QStringLiteral("date")).toString());
            result.append(item);
        }
    } else if (provider == QStringLiteral("flutter") && document.isObject()) {
        for (const QJsonValue &value :
             document.object().value(QStringLiteral("releases")).toArray()) {
            const QJsonObject release = value.toObject();
            const QString arch = release.value(QStringLiteral("dart_sdk_arch")).toString();
            if (!arch.isEmpty() && arch != QStringLiteral("x64"))
                continue;
            QVariantMap item;
            item.insert(QStringLiteral("version"),
                        release.value(QStringLiteral("version")).toString());
            item.insert(QStringLiteral("channel"),
                        release.value(QStringLiteral("channel")).toString());
            item.insert(QStringLiteral("date"),
                        release.value(QStringLiteral("release_date")).toString().left(10));
            result.append(item);
        }
    } else if (provider == QStringLiteral("java") && document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            const QJsonObject release = value.toObject();
            const QJsonObject versionData =
                release.value(QStringLiteral("version_data")).toObject();
            const QString version = QStringLiteral("%1.%2.%3")
                .arg(versionData.value(QStringLiteral("major")).toInt())
                .arg(versionData.value(QStringLiteral("minor")).toInt())
                .arg(versionData.value(QStringLiteral("security")).toInt());
            if (version.startsWith(QStringLiteral("0.")))
                continue;
            const int major = versionData.value(QStringLiteral("major")).toInt();
            QVariantMap item;
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"),
                        QList<int>{8, 11, 17, 21, 25}.contains(major)
                            ? QStringLiteral("LTS") : QStringLiteral("stable"));
            item.insert(QStringLiteral("date"),
                        release.value(QStringLiteral("release_date")).toString().left(10));
            result.append(item);
        }
    } else if (provider == QStringLiteral("python") && document.isArray()) {
        static const QRegularExpression pattern(
            QStringLiteral(R"(/python/(\d+\.\d+\.\d+)/python-\1-amd64\.exe$)"));
        for (const QJsonValue &value : document.array()) {
            const QJsonObject fileObject = value.toObject();
            if (fileObject.value(QStringLiteral("name")).toString()
                != QStringLiteral("Windows installer (64-bit)")) {
                continue;
            }
            const QRegularExpressionMatch match =
                pattern.match(fileObject.value(QStringLiteral("url")).toString());
            if (!match.hasMatch()) {
                continue;
            }
            const QStringList versionParts = match.captured(1).split(u'.');
            if (versionParts.value(0).toInt() != 3 || versionParts.value(1).toInt() < 10)
                continue;
            QVariantMap item;
            item.insert(QStringLiteral("version"), match.captured(1));
            item.insert(QStringLiteral("channel"), QStringLiteral("stable"));
            item.insert(QStringLiteral("date"), QStringLiteral("—"));
            result.append(item);
        }
    } else if (provider == QStringLiteral("php") && document.isObject()) {
        const QJsonObject documentRoot = document.object();
        const QJsonObject root = documentRoot.contains(QStringLiteral("current"))
            ? documentRoot.value(QStringLiteral("current")).toObject()
            : documentRoot;
        QSet<QString> knownVersions;
        for (auto releaseIt = root.constBegin(); releaseIt != root.constEnd(); ++releaseIt) {
            const QJsonObject release = releaseIt.value().toObject();
            const QString version = release.value(QStringLiteral("version")).toString();
            const QStringList parts = version.split(u'.');
            if (parts.size() < 3 || parts[0].toInt() != 8 || parts[1].toInt() < 1)
                continue;
            for (const QString &buildType :
                 {QStringLiteral("nts"), QStringLiteral("ts")}) {
                QJsonObject build;
                for (auto buildIt = release.constBegin(); buildIt != release.constEnd();
                     ++buildIt) {
                    const bool matchingType = buildType == QStringLiteral("nts")
                        ? buildIt.key().startsWith(QStringLiteral("nts-"))
                        : buildIt.key().startsWith(QStringLiteral("ts-"));
                    if (matchingType && buildIt.key().endsWith(QStringLiteral("-x64"))) {
                        build = buildIt.value().toObject();
                        break;
                    }
                }
                const QJsonObject archive = build.value(QStringLiteral("zip")).toObject();
                if (archive.value(QStringLiteral("path")).toString().isEmpty()
                    || archive.value(QStringLiteral("sha256")).toString().size() != 64) {
                    continue;
                }
                QVariantMap item;
                item.insert(QStringLiteral("version"), version + u'-' + buildType);
                item.insert(QStringLiteral("channel"),
                            parts[1].toInt() >= 4 ? QStringLiteral("stable")
                                                  : QStringLiteral("security"));
                item.insert(QStringLiteral("buildType"), buildType);
                item.insert(QStringLiteral("date"),
                            build.value(QStringLiteral("mtime")).toString().left(10));
                result.append(item);
                knownVersions.insert(item.value(QStringLiteral("version")).toString());
            }
        }
        static const QRegularExpression archivePattern(
            QStringLiteral(
                R"(php-(\d+\.\d+\.\d+)(-nts)?-Win32-[A-Za-z0-9]+-x64\.zip)"),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator matches = archivePattern.globalMatch(
            documentRoot.value(QStringLiteral("archiveHtml")).toString());
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString baseVersion = match.captured(1);
            const QString buildType =
                match.captured(2).isEmpty() ? QStringLiteral("ts") : QStringLiteral("nts");
            const QString version = baseVersion + u'-' + buildType;
            if (knownVersions.contains(version))
                continue;
            QVariantMap item;
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"), QStringLiteral("legacy"));
            item.insert(QStringLiteral("buildType"), buildType);
            item.insert(QStringLiteral("date"), QStringLiteral("—"));
            result.append(item);
            knownVersions.insert(version);
        }
    } else if (provider == QStringLiteral("go") && document.isArray()) {
        static const QRegularExpression versionPattern(
            QStringLiteral(R"(^go(\d+)\.(\d+)(?:\.(\d+))?(.*)$)"));
        for (const QJsonValue &value : document.array()) {
            const QJsonObject release = value.toObject();
            const QRegularExpressionMatch match =
                versionPattern.match(release.value(QStringLiteral("version")).toString());
            if (!match.hasMatch())
                continue;
            bool hasWindowsArchive = false;
            for (const QJsonValue &fileValue : release.value(QStringLiteral("files")).toArray()) {
                const QJsonObject fileObject = fileValue.toObject();
                if (fileObject.value(QStringLiteral("os")).toString() == QStringLiteral("windows")
                    && fileObject.value(QStringLiteral("arch")).toString()
                           == QStringLiteral("amd64")
                    && fileObject.value(QStringLiteral("kind")).toString()
                           == QStringLiteral("archive")
                    && fileObject.value(QStringLiteral("sha256")).toString().size() == 64) {
                    hasWindowsArchive = true;
                    break;
                }
            }
            if (!hasWindowsArchive)
                continue;
            QString version = QStringLiteral("%1.%2.%3")
                                  .arg(match.captured(1), match.captured(2),
                                       match.captured(3).isEmpty()
                                           ? QStringLiteral("0") : match.captured(3));
            if (!match.captured(4).isEmpty())
                version += u'-' + match.captured(4);
            QVariantMap item;
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"),
                        release.value(QStringLiteral("stable")).toBool()
                            ? QStringLiteral("stable") : QStringLiteral("beta"));
            item.insert(QStringLiteral("date"), QStringLiteral("—"));
            result.append(item);
        }
    } else if (provider == QStringLiteral("maven")) {
        const QString html = QString::fromUtf8(data);
        static const QRegularExpression pattern(
            QStringLiteral(R"(>(3\.(?:8|9|10)\.\d+|4\.0\.0-(?:alpha|beta|rc)-\d+)<)"),
            QRegularExpression::CaseInsensitiveOption);
        QSet<QString> seen;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(html);
        while (matches.hasNext()) {
            const QString version = matches.next().captured(1).toLower();
            if (seen.contains(version))
                continue;
            seen.insert(version);
            QVariantMap item;
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"),
                        version.startsWith(QStringLiteral("4."))
                            ? QStringLiteral("preview") : QStringLiteral("stable"));
            item.insert(QStringLiteral("date"), QStringLiteral("—"));
            result.append(item);
        }
    } else if (provider == QStringLiteral("postgresql")) {
        for (const QJsonValue &value : document.array()) {
            const QJsonObject release = value.toObject();
            if (!release.value(QStringLiteral("supported")).toBool())
                continue;
            const QString major = release.value(QStringLiteral("major")).toString();
            const QString minor = release.value(QStringLiteral("latestMinor")).toString();
            if (major.toInt() < 13 || minor.isEmpty())
                continue;
            const QString version = major + u'.' + minor;
            QVariantMap item;
            item.insert(QStringLiteral("version"), version);
            item.insert(QStringLiteral("channel"), QStringLiteral("stable"));
            item.insert(QStringLiteral("date"),
                        release.value(QStringLiteral("relDate")).toString());
            result.append(item);
        }
    }
    return result;
}

bool downloadedLocally(const QString &provider, const QString &version)
{
    const QString directory =
        dataRoot() + QStringLiteral("/downloads/") + provider + u'/' + version;
    if (QFileInfo(directory + QStringLiteral("/download.json")).isFile())
        return true;
    if (provider == QStringLiteral("node")) {
        return QFileInfo(directory + QStringLiteral("/node-v") + version
                         + QStringLiteral("-win-x64.zip")).isFile();
    }
    return false;
}

int commandList(const QStringList &arguments)
{
    static const QStringList providers{QStringLiteral("node"), QStringLiteral("flutter"),
                                       QStringLiteral("java"), QStringLiteral("python"),
                                       QStringLiteral("php"), QStringLiteral("go"),
                                       QStringLiteral("postgresql"), QStringLiteral("maven")};
    if (arguments.isEmpty()) {
        const QJsonObject defaults =
            readObject(dataRoot() + QStringLiteral("/settings/default-versions.json"))
                .value(QStringLiteral("providers")).toObject();
        out << "Provider   Cached versions   Default" << Qt::endl;
        for (const QString &provider : providers) {
            const QVariantList versions = cachedVersions(provider);
            out << provider.leftJustified(10) << QString::number(versions.size()).leftJustified(18)
                << defaults.value(provider).toString(QStringLiteral("-")) << Qt::endl;
        }
        return 0;
    }

    QString filter = QStringLiteral("all");
    QString provider;
    if (arguments.size() == 1) {
        provider = arguments[0].toLower();
    } else if (arguments.size() == 2) {
        filter = arguments[0].toLower();
        provider = arguments[1].toLower();
    } else {
        err << "Usage: svm list [filter] [provider]" << Qt::endl;
        return 2;
    }
    if (!providers.contains(provider)) {
        err << "Unsupported provider: " << provider << Qt::endl;
        return 2;
    }
    const QStringList filters{QStringLiteral("all"), QStringLiteral("lts"),
                              QStringLiteral("current"), QStringLiteral("stable"),
                              QStringLiteral("beta"), QStringLiteral("nts"),
                              QStringLiteral("ts"), QStringLiteral("legacy"),
                              QStringLiteral("preview"),
                              QStringLiteral("downloaded")};
    if (!filters.contains(filter)) {
        err << "Unknown filter: " << filter
            << ". Use all, lts, current, stable, beta, preview, nts, ts, legacy, or downloaded."
            << Qt::endl;
        return 2;
    }

    const QVariantList versions = cachedVersions(provider);
    if (versions.isEmpty()) {
        err << "No cached " << provider
            << " version index. Open it in the GUI or click Refresh versions first." << Qt::endl;
        return 3;
    }
    const QString defaultVersion =
        readObject(dataRoot() + QStringLiteral("/settings/default-versions.json"))
            .value(QStringLiteral("providers")).toObject().value(provider).toString();
    out << "Version                  Channel              Date         Status" << Qt::endl;
    int shown = 0;
    for (const QVariant &entry : versions) {
        const QVariantMap item = entry.toMap();
        const QString version = item.value(QStringLiteral("version")).toString();
        const QString channel = item.value(QStringLiteral("channel")).toString();
        const bool downloaded = downloadedLocally(provider, version);
        const QString normalizedChannel = channel.toLower();
        const QString buildType = item.value(QStringLiteral("buildType")).toString().toLower();
        const bool matches =
            filter == QStringLiteral("all")
            || (filter == QStringLiteral("lts") && normalizedChannel.contains(QStringLiteral("lts")))
            || (filter == QStringLiteral("current")
                && normalizedChannel.contains(QStringLiteral("current")))
            || (filter == QStringLiteral("stable") && normalizedChannel == QStringLiteral("stable"))
            || (filter == QStringLiteral("beta") && normalizedChannel == QStringLiteral("beta"))
            || (filter == QStringLiteral("preview") && normalizedChannel == QStringLiteral("preview"))
            || (filter == QStringLiteral("legacy")
                && normalizedChannel == QStringLiteral("legacy"))
            || (filter == QStringLiteral("nts") && buildType == QStringLiteral("nts"))
            || (filter == QStringLiteral("ts") && buildType == QStringLiteral("ts"))
            || (filter == QStringLiteral("downloaded") && downloaded);
        if (!matches)
            continue;
        QString status;
        if (version == defaultVersion)
            status = QStringLiteral("default");
        else if (downloaded)
            status = QStringLiteral("downloaded");
        else
            status = QStringLiteral("-");
        out << version.leftJustified(25)
            << channel.leftJustified(21)
            << item.value(QStringLiteral("date")).toString().leftJustified(13)
            << status << Qt::endl;
        ++shown;
    }
    out << shown << " version(s)" << Qt::endl;
    return 0;
}

int commandProxy(const QStringList &arguments)
{
    if (arguments.isEmpty() || arguments == QStringList{QStringLiteral("show")}) {
        const QString url = configuredProxyUrl();
        out << (url.isEmpty() ? QStringLiteral("Proxy: disabled")
                             : QStringLiteral("Proxy: %1").arg(url))
            << Qt::endl;
        return 0;
    }
    if (arguments == QStringList{QStringLiteral("clear")}) {
        QString errorMessage;
        if (!clearConfiguredProxy(&errorMessage)) {
            err << errorMessage << Qt::endl;
            return 1;
        }
        out << "Proxy cleared." << Qt::endl;
        return 0;
    }
    if (arguments.size() == 2 && arguments[0] == QStringLiteral("set")) {
        QString errorMessage;
        if (!setConfiguredProxyUrl(arguments[1], &errorMessage)) {
            err << errorMessage << Qt::endl;
            return 2;
        }
        out << "Proxy set to " << arguments[1] << Qt::endl;
        return 0;
    }
    err << "Usage: svm proxy [show|set <http-url>|clear]" << Qt::endl;
    return 2;
}

int commandInstall(const QStringList &arguments)
{
    if (arguments.size() != 2 || !validToken(arguments[0]) || !validToken(arguments[1])) {
        err << "Usage: svm install <provider> <version>" << Qt::endl;
        return 2;
    }
    const QString provider = arguments[0].toLower();
    const QString version = arguments[1];
    if (installedExecutable(provider, version).isEmpty()) {
        err << "Unsupported provider: " << provider << Qt::endl;
        return 2;
    }
    QString javaVersion;
    if (provider == QStringLiteral("maven")) {
        javaVersion = ensureMavenJava(version);
        if (javaVersion.isEmpty())
            return 3;
    }
    if (!QFileInfo(installedExecutable(provider, version)).isFile()
        && !downloadAndInstall(provider, version, false, verboseOutput))
        return 3;
    if (!javaVersion.isEmpty()) {
        if (!saveMavenRuntimeJava(version, javaVersion)) {
            err << "Maven was installed, but its Java runtime association could not be saved."
                << Qt::endl;
            return 1;
        }
        out << "Configured Maven " << version << " to run with Java " << javaVersion << '.'
            << Qt::endl;
    }
    return 0;
}

QString quotePowerShell(const QString &value)
{
    QString escaped = value;
    escaped.replace(u'\'', QStringLiteral("''"));
    return u'\'' + escaped + u'\'';
}

int commandEnvironment(const QStringList &arguments)
{
    if (arguments != QStringList{QStringLiteral("powershell")}) {
        err << "Usage: svm env powershell" << Qt::endl;
        return 2;
    }

    out << "if (-not $env:SVM_ENV_INITIALIZED) {" << Qt::endl;
    out << "  $env:SVM_BASE_PATH = $env:PATH" << Qt::endl;
    out << "  $env:SVM_BASE_JAVA_HOME = $env:JAVA_HOME" << Qt::endl;
    out << "  $env:SVM_BASE_GOROOT = $env:GOROOT" << Qt::endl;
    out << "  $env:SVM_BASE_PHPRC = $env:PHPRC" << Qt::endl;
    out << "  $env:SVM_ENV_INITIALIZED = '1'" << Qt::endl;
    out << "}" << Qt::endl;
    out << "$env:PATH = $env:SVM_BASE_PATH" << Qt::endl;
    out << "if ($env:SVM_BASE_JAVA_HOME) { $env:JAVA_HOME = $env:SVM_BASE_JAVA_HOME } "
           "else { Remove-Item Env:JAVA_HOME -ErrorAction SilentlyContinue }" << Qt::endl;
    out << "if ($env:SVM_BASE_GOROOT) { $env:GOROOT = $env:SVM_BASE_GOROOT } "
           "else { Remove-Item Env:GOROOT -ErrorAction SilentlyContinue }" << Qt::endl;
    out << "if ($env:SVM_BASE_PHPRC) { $env:PHPRC = $env:SVM_BASE_PHPRC } "
           "else { Remove-Item Env:PHPRC -ErrorAction SilentlyContinue }" << Qt::endl;
    out << "Remove-Item Env:SVM_PROJECT_ROOT -ErrorAction SilentlyContinue" << Qt::endl;

    const QString projectRoot = findProjectRoot(QDir::currentPath());
    if (projectRoot.isEmpty())
        return 0;

    const QJsonObject sdks = readObject(projectConfigPath(projectRoot))
                                  .value(QStringLiteral("sdks")).toObject();
    QStringList paths;
    for (auto it = sdks.constBegin(); it != sdks.constEnd(); ++it) {
        const QString provider = it.key();
        const QString version = it.value().toString();
        const QString root = installedVersionDirectory(provider, version);
        const QString executable = installedExecutable(provider, version);
        if (!validToken(provider) || !validToken(version) || !QFileInfo(executable).isFile()) {
            err << "Cannot activate " << provider << ' ' << version
                << ": the managed installation is missing." << Qt::endl;
            return 3;
        }
        if (provider == QStringLiteral("node") || provider == QStringLiteral("python")
            || provider == QStringLiteral("php")) {
            paths.append(root);
        } else {
            paths.append(QDir(root).filePath(QStringLiteral("bin")));
        }
        if (provider == QStringLiteral("python"))
            paths.append(QDir(root).filePath(QStringLiteral("Scripts")));
        if (provider == QStringLiteral("java"))
            out << "$env:JAVA_HOME = " << quotePowerShell(QDir::toNativeSeparators(root))
                << Qt::endl;
        else if (provider == QStringLiteral("go"))
            out << "$env:GOROOT = " << quotePowerShell(QDir::toNativeSeparators(root))
                << Qt::endl;
        else if (provider == QStringLiteral("php"))
            out << "$env:PHPRC = " << quotePowerShell(QDir::toNativeSeparators(root))
                << Qt::endl;
    }
    paths.removeDuplicates();
    for (QString &path : paths)
        path = QDir::toNativeSeparators(path);
    if (!paths.isEmpty()) {
        out << "$env:PATH = " << quotePowerShell(paths.join(u';') + u';')
            << " + $env:SVM_BASE_PATH" << Qt::endl;
    }
    out << "$env:SVM_PROJECT_ROOT = "
        << quotePowerShell(QDir::toNativeSeparators(projectRoot)) << Qt::endl;
    return 0;
}

QString powerShellSingleQuoted(const QString &value)
{
    QString escaped = value;
    escaped.replace(u'\'', QStringLiteral("''"));
    return u'\'' + escaped + u'\'';
}

bool writeBytesAtomically(const QString &path, const QByteArray &content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(content) == content.size()
        && file.commit()) {
        return true;
    }
    file.cancelWriting();

    // Some Windows profile files cannot be atomically replaced while PowerShell
    // startup is reading them. Preserve a recovery copy before the in-place fallback.
    const QString backupPath = path + QStringLiteral(".svm-backup");
    if (QFileInfo(path).isFile() && !QFileInfo(backupPath).exists()
        && !QFile::copy(path, backupPath)) {
        return false;
    }
    QFile fallback(path);
    return fallback.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && fallback.write(content) == content.size() && fallback.flush();
}

int commandShell(const QStringList &arguments)
{
    if (arguments.size() != 2 || arguments[1] != QStringLiteral("powershell")
        || (arguments[0] != QStringLiteral("install")
            && arguments[0] != QStringLiteral("uninstall"))) {
        err << "Usage: svm shell <install|uninstall> powershell" << Qt::endl;
        return 2;
    }

    const QString hookPath = dataRoot() + QStringLiteral("/shell/svm-hook.ps1");
    const QByteArray marker("# SVM automatic environment hook");
    const QString loadLine = QStringLiteral(". %1 %2")
                                 .arg(powerShellSingleQuoted(QDir::toNativeSeparators(hookPath)),
                                      QString::fromLatin1(marker));
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QStringList profiles{
        QDir(documents).filePath(QStringLiteral("WindowsPowerShell/profile.ps1")),
        QDir(documents).filePath(QStringLiteral("PowerShell/profile.ps1"))
    };

    if (arguments[0] == QStringLiteral("install")) {
        const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString hook = QStringLiteral(
            "# Managed by SDK Version Manager. Changes may be replaced.\r\n"
            "if (-not $global:SvmEnvironmentHookLoaded) {\r\n"
            "    $global:SvmEnvironmentHookLoaded = $true\r\n"
            "    $script:SvmExecutable = %1\r\n"
            "    $script:SvmPreviousPrompt = $function:prompt\r\n"
            "    function global:Update-SvmEnvironment {\r\n"
            "        $environmentScript = @(& $script:SvmExecutable env powershell)\r\n"
            "        if ($LASTEXITCODE -eq 0 -and $environmentScript.Count -gt 0) {\r\n"
            "            Invoke-Expression ($environmentScript -join \"`n\")\r\n"
            "        }\r\n"
            "    }\r\n"
            "    function global:prompt {\r\n"
            "        Update-SvmEnvironment\r\n"
            "        if ($script:SvmPreviousPrompt) { & $script:SvmPreviousPrompt }\r\n"
            "        else { \"PS $($ExecutionContext.SessionState.Path.CurrentLocation)> \" }\r\n"
            "    }\r\n"
            "    Update-SvmEnvironment\r\n"
            "}\r\n").arg(powerShellSingleQuoted(executable));
        if (!writeBytesAtomically(hookPath, hook.toUtf8())) {
            err << "Failed to write PowerShell hook: " << hookPath << Qt::endl;
            return 1;
        }
    }

    for (const QString &profilePath : profiles) {
        QFile file(profilePath);
        QByteArray content;
        if (file.open(QIODevice::ReadOnly))
            content = file.readAll();
        QList<QByteArray> lines = content.replace("\r\n", "\n").split('\n');
        for (qsizetype i = lines.size() - 1; i >= 0; --i) {
            if (lines[i].contains(marker))
                lines.removeAt(i);
        }
        while (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        if (arguments[0] == QStringLiteral("install"))
            lines.append(loadLine.toUtf8());
        QByteArray updated = lines.join("\r\n");
        if (!updated.isEmpty())
            updated.append("\r\n");
        if (!writeBytesAtomically(profilePath, updated)) {
            err << "Failed to update PowerShell profile: " << profilePath << Qt::endl;
            return 1;
        }
        if (arguments[0] == QStringLiteral("uninstall"))
            QFile::remove(profilePath + QStringLiteral(".svm-backup"));
    }

    if (arguments[0] == QStringLiteral("uninstall")) {
        QFile::remove(hookPath);
        QDir().rmdir(QFileInfo(hookPath).absolutePath());
    }
    out << "PowerShell automatic environment hook " << arguments[0] << "ed." << Qt::endl;
    return 0;
}

QString quoteForCmd(const QString &argument)
{
    QString escaped = argument;
    escaped.replace(u'"', QStringLiteral("\"\""));
    return u'"' + escaped + u'"';
}

int proxyCommand(const ProviderEntryPoint &entryPoint, const QStringList &arguments)
{
    const QString provider = entryPoint.provider;
    QString source;
    const QString version = resolveVersion(provider, QDir::currentPath(), &source);
    if (version.isEmpty()) {
        err << "No active " << provider << " version. Use `svm use " << provider
            << " <version>` or set a global default in the GUI." << Qt::endl;
        return 3;
    }
    const QString executable = QDir(installedVersionDirectory(provider, version))
                                   .filePath(entryPoint.relativeExecutable);
    if (!QFileInfo(executable).isFile()) {
        err << provider << ' ' << version << " is configured but not installed: "
            << executable << Qt::endl;
        return 3;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setWorkingDirectory(QDir::currentPath());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString sdkBin = QFileInfo(executable).absolutePath();
    QStringList sdkPaths{QDir::toNativeSeparators(sdkBin)};
    if (provider == QStringLiteral("python"))
        sdkPaths.append(QDir::toNativeSeparators(sdkBin + QStringLiteral("/Scripts")));
    if (provider == QStringLiteral("maven")) {
        QString javaVersion;
        const QString projectRoot = findProjectRoot(QDir::currentPath());
        if (!projectRoot.isEmpty()) {
            javaVersion = readObject(projectConfigPath(projectRoot))
                              .value(QStringLiteral("sdks")).toObject()
                              .value(QStringLiteral("java")).toString();
        }
        if (javaVersion.isEmpty())
            javaVersion = mavenRuntimeJava(version);
        if (javaVersion.isEmpty())
            javaVersion = resolveVersion(QStringLiteral("java"), QDir::currentPath(), nullptr);
        if (!isInstalledJavaCompatible(javaVersion, minimumJavaForMaven(version))) {
            err << "Maven " << version << " requires an installed Java "
                << minimumJavaForMaven(version) << "+ runtime. Run `svm use maven "
                << version << "` to resolve it automatically." << Qt::endl;
            return 3;
        }
        const QString javaHome = installedVersionDirectory(QStringLiteral("java"), javaVersion);
        sdkPaths.append(QDir::toNativeSeparators(javaHome + QStringLiteral("/bin")));
        environment.insert(QStringLiteral("JAVA_HOME"), QDir::toNativeSeparators(javaHome));
        environment.insert(QStringLiteral("MAVEN_HOME"),
                           QDir::toNativeSeparators(installedVersionDirectory(provider, version)));
    }
    environment.insert(QStringLiteral("PATH"),
                       sdkPaths.join(u';') + u';'
                           + environment.value(QStringLiteral("PATH")));
    process.setProcessEnvironment(environment);
    if (provider == QStringLiteral("java")) {
        environment.insert(QStringLiteral("JAVA_HOME"),
                           installedVersionDirectory(provider, version));
        process.setProcessEnvironment(environment);
    }
    if (provider == QStringLiteral("php")) {
        environment.insert(QStringLiteral("PHPRC"), installedVersionDirectory(provider, version));
        process.setProcessEnvironment(environment);
    }
    if (provider == QStringLiteral("go")) {
        environment.insert(QStringLiteral("GOROOT"), installedVersionDirectory(provider, version));
        process.setProcessEnvironment(environment);
    }

    QStringList processArguments;
    for (const QString &leadingArgument : entryPoint.leadingArguments) {
        const QString candidate = QDir(installedVersionDirectory(provider, version))
                                      .filePath(leadingArgument);
        processArguments.append(QFileInfo(candidate).isFile() ? candidate : leadingArgument);
    }
    processArguments.append(arguments);

    if (!entryPoint.requiresCommandInterpreter) {
        process.start(executable, processArguments);
    } else {
        for (const QString &argument : processArguments) {
            if (argument.contains(QRegularExpression(QStringLiteral(R"([%!\^&|<>()])")))) {
                err << "Unsafe shell metacharacter in batch-file argument." << Qt::endl;
                return 2;
            }
        }
        QString command = quoteForCmd(QDir::toNativeSeparators(executable));
        for (const QString &argument : processArguments)
            command += u' ' + quoteForCmd(argument);
#ifdef Q_OS_WIN
        process.setProgram(QStringLiteral("cmd.exe"));
        // cmd /s /c requires an additional outer quote pair when the command
        // itself starts with a quoted executable path.
        process.setNativeArguments(QStringLiteral("/d /s /c \"") + command + u'"');
        process.start();
#else
        process.start(executable, arguments);
#endif
    }
    if (!process.waitForStarted()) {
        err << "Failed to start " << executable << ": " << process.errorString() << Qt::endl;
        return 1;
    }
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}

void printHelp()
{
    out << "SVM - SDK Version Manager\n\n"
           "Usage:\n"
           "  svm -v <command> [arguments...]\n"
           "  svm init\n"
           "  svm install <provider> <version>\n"
           "  svm use [provider [version]]\n"
           "  svm use php <legacy-version> --allow-unverified-archive\n"
           "  svm ide [vscode|idea|android-studio]\n"
           "  svm list [filter] [provider]\n"
           "  svm proxy [show|set <http-url>|clear]\n"
           "  svm env powershell\n"
           "  svm shell <install|uninstall> powershell\n"
           "  svm <provider|entry-point> [arguments...]\n\n"
           "Examples:\n"
           "  svm -v use java 17.0.19\n"
           "  svm use node 24.18.0\n"
           "  svm install maven 3.9.16\n"
           "  svm ide vscode\n"
           "  svm list lts node\n"
           "  svm proxy set http://127.0.0.1:7890\n"
           "  svm node --version\n"
           "  svm npm --version\n"
           "  svm npx --version\n"
           "  svm java --version\n"
           "  svm python --version\n"
           "  svm php --version\n"
           "  svm go version\n"
           "  svm postgresql --version\n"
           "  svm use php 5.6.40-nts --allow-unverified-archive\n"
           "  svm flutter doctor\n";
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("appsdk_version_manager"));

    QStringList arguments = application.arguments();
    arguments.removeFirst();
    if (!arguments.isEmpty()
        && (arguments.first() == QStringLiteral("-v")
            || arguments.first() == QStringLiteral("--verbose"))) {
        verboseOutput = true;
        arguments.removeFirst();
    }
    if (arguments.isEmpty() || arguments.first() == QStringLiteral("--help")
        || arguments.first() == QStringLiteral("-h")) {
        printHelp();
        return 0;
    }

    const QString command = arguments.takeFirst().toLower();
    if (command == QStringLiteral("init"))
        return commandInit(arguments);
    if (command == QStringLiteral("install"))
        return commandInstall(arguments);
    if (command == QStringLiteral("use"))
        return commandUse(arguments);
    if (command == QStringLiteral("ide"))
        return commandIde(arguments);
    if (command == QStringLiteral("list"))
        return commandList(arguments);
    if (command == QStringLiteral("proxy"))
        return commandProxy(arguments);
    if (command == QStringLiteral("env"))
        return commandEnvironment(arguments);
    if (command == QStringLiteral("shell"))
        return commandShell(arguments);
    ProviderEntryPoint entryPoint;
    if (providerEntryPoint(command, &entryPoint))
        return proxyCommand(entryPoint, arguments);

    err << "Unknown command or provider: " << command << Qt::endl;
    printHelp();
    return 2;
}
