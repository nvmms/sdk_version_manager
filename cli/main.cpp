#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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
#include "../src/providercontroller.h"

namespace {
QTextStream out(stdout);
QTextStream err(stderr);
QTextStream input(stdin);

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
    return {};
}

QString installedVersionDirectory(const QString &provider, const QString &version)
{
    return QDir::cleanPath(dataRoot() + QStringLiteral("/installs/") + provider + u'/' + version);
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
                        bool allowUnverifiedArchive = false)
{
    ProviderController controller;
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

    if (provider == QStringLiteral("flutter") || provider == QStringLiteral("java")
        || provider == QStringLiteral("python") || provider == QStringLiteral("php")
        || provider == QStringLiteral("go") || provider == QStringLiteral("postgresql")) {
        out << "Reading the " << provider << " version index..." << Qt::endl;
        controller.loadVersions(provider, false);
        waitUntilIdle(controller);
        if (!controller.error().isEmpty()) {
            publish(QStringLiteral("error"));
            err << "Failed to load the " << provider << " version index." << Qt::endl;
            return false;
        }
    }

    bool unverifiedArchive = false;
    for (const QVariant &entry : controller.versions()) {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("version")).toString() == version) {
            unverifiedArchive = item.value(QStringLiteral("unverified")).toBool();
            break;
        }
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
    QFile existingIgnore(ignorePath);
    QByteArray ignoreContent;
    if (existingIgnore.open(QIODevice::ReadOnly))
        ignoreContent = existingIgnore.readAll();
    const QList<QByteArray> ignoreLines = ignoreContent.replace("\r\n", "\n").split('\n');
    if (!ignoreLines.contains(QByteArrayLiteral(".svm/"))) {
        if (!ignoreContent.isEmpty() && !ignoreContent.endsWith('\n'))
            ignoreContent.append('\n');
        ignoreContent.append(".svm/\n");
        QSaveFile ignore(ignorePath);
        if (!ignore.open(QIODevice::WriteOnly)) {
            err << "Initialized config, but failed to update " << ignorePath << Qt::endl;
            return 1;
        }
        ignore.write(ignoreContent);
        if (!ignore.commit()) {
            err << "Initialized config, but failed to update " << ignorePath << Qt::endl;
            return 1;
        }
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
    if (root.isEmpty()) {
        const int initResult = commandInit({});
        if (initResult != 0)
            return initResult;
        root = QDir::currentPath();
    }

    const QString provider = arguments[0].toLower();
    const QString version = arguments[1];
    const QString executable = installedExecutable(provider, version);
    if (executable.isEmpty()) {
        err << "Unsupported provider: " << provider << Qt::endl;
        return 2;
    }
    if (!QFileInfo(executable).isFile()) {
        if (!downloadAndInstall(provider, version, allowUnverifiedArchive))
            return 3;
    }

    if (!writeProjectBinding(root, provider, version)) {
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
    const QJsonObject sdks = readObject(projectConfigPath(root))
                                  .value(QStringLiteral("sdks")).toObject();
    out << provider << ' ' << version << " is now active for " << root << Qt::endl;
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
                                       QStringLiteral("postgresql")};
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
                              QStringLiteral("downloaded")};
    if (!filters.contains(filter)) {
        err << "Unknown filter: " << filter
            << ". Use all, lts, current, stable, beta, nts, ts, legacy, or downloaded."
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

QString quoteForCmd(const QString &argument)
{
    QString escaped = argument;
    escaped.replace(u'"', QStringLiteral("\"\""));
    return u'"' + escaped + u'"';
}

int proxyCommand(const QString &provider, const QStringList &arguments)
{
    QString source;
    const QString version = resolveVersion(provider, QDir::currentPath(), &source);
    if (version.isEmpty()) {
        err << "No active " << provider << " version. Use `svm use " << provider
            << " <version>` or set a global default in the GUI." << Qt::endl;
        return 3;
    }
    const QString executable = installedExecutable(provider, version);
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

    if (provider == QStringLiteral("node") || provider == QStringLiteral("java")
        || provider == QStringLiteral("python") || provider == QStringLiteral("php")
        || provider == QStringLiteral("go") || provider == QStringLiteral("postgresql")) {
        process.start(executable, arguments);
    } else {
        for (const QString &argument : arguments) {
            if (argument.contains(QRegularExpression(QStringLiteral(R"([%!\^&|<>()])")))) {
                err << "Unsafe shell metacharacter in Flutter argument." << Qt::endl;
                return 2;
            }
        }
        QString command = quoteForCmd(QDir::toNativeSeparators(executable));
        for (const QString &argument : arguments)
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
           "  svm init\n"
           "  svm use [provider [version]]\n"
           "  svm use php <legacy-version> --allow-unverified-archive\n"
           "  svm ide [vscode|idea|android-studio]\n"
           "  svm list [filter] [provider]\n"
           "  svm <provider> [arguments...]\n\n"
           "Examples:\n"
           "  svm use node 24.18.0\n"
           "  svm ide vscode\n"
           "  svm list lts node\n"
           "  svm node --version\n"
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
    if (arguments.isEmpty() || arguments.first() == QStringLiteral("--help")
        || arguments.first() == QStringLiteral("-h")) {
        printHelp();
        return 0;
    }

    const QString command = arguments.takeFirst().toLower();
    if (command == QStringLiteral("init"))
        return commandInit(arguments);
    if (command == QStringLiteral("use"))
        return commandUse(arguments);
    if (command == QStringLiteral("ide"))
        return commandIde(arguments);
    if (command == QStringLiteral("list"))
        return commandList(arguments);
    if (command == QStringLiteral("node") || command == QStringLiteral("flutter")
        || command == QStringLiteral("java") || command == QStringLiteral("python")
        || command == QStringLiteral("php") || command == QStringLiteral("go")
        || command == QStringLiteral("postgresql"))
        return proxyCommand(command, arguments);

    err << "Unknown command or provider: " << command << Qt::endl;
    printHelp();
    return 2;
}
