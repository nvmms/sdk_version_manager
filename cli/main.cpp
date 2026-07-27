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
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVersionNumber>

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
    return {};
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
        if (best.isEmpty() || QVersionNumber::compare(number, bestNumber) > 0
            || (QVersionNumber::compare(number, bestNumber) == 0 && version > best)) {
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

bool downloadAndInstall(const QString &provider, const QString &version)
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

    if (provider == QStringLiteral("flutter")) {
        out << "Reading the Flutter version index..." << Qt::endl;
        controller.loadVersions(provider, false);
        waitUntilIdle(controller);
        if (!controller.error().isEmpty()) {
            publish(QStringLiteral("error"));
            err << "Failed to load the Flutter version index." << Qt::endl;
            return false;
        }
    }

    const QString displayName =
        provider == QStringLiteral("node") ? QStringLiteral("Node.js") : QStringLiteral("Flutter");
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
    controller.download(provider, version);
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

int commandUse(const QStringList &arguments)
{
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
        return commandUse({provider, version});
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
        if (!downloadAndInstall(provider, version))
            return 3;
    }

    if (!writeProjectBinding(root, provider, version)) {
        err << "Failed to update " << projectConfigPath(root) << Qt::endl;
        return 1;
    }
    out << provider << ' ' << version << " is now active for " << root << Qt::endl;
    return 0;
}

int commandActive()
{
    const QString projectRoot = findProjectRoot(QDir::currentPath());
    if (!projectRoot.isEmpty()) {
        const QJsonObject sdks = readObject(projectConfigPath(projectRoot))
                                      .value(QStringLiteral("sdks")).toObject();
        out << "Project: " << projectRoot << Qt::endl;
        if (sdks.isEmpty())
            out << "  No project SDK bindings." << Qt::endl;
        for (auto it = sdks.constBegin(); it != sdks.constEnd(); ++it)
            out << "  " << it.key() << ": " << it.value().toString() << " (project)" << Qt::endl;
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
    return 0;
}

QVariantList cachedVersions(const QString &provider)
{
    const QString path =
        dataRoot() + QStringLiteral("/cache/") + provider + QStringLiteral("/versions.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
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
    static const QStringList providers{QStringLiteral("node"), QStringLiteral("flutter")};
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
                              QStringLiteral("beta"), QStringLiteral("downloaded")};
    if (!filters.contains(filter)) {
        err << "Unknown filter: " << filter
            << ". Use all, lts, current, stable, beta, or downloaded." << Qt::endl;
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
        const bool matches =
            filter == QStringLiteral("all")
            || (filter == QStringLiteral("lts") && normalizedChannel.contains(QStringLiteral("lts")))
            || (filter == QStringLiteral("current")
                && normalizedChannel.contains(QStringLiteral("current")))
            || (filter == QStringLiteral("stable") && normalizedChannel == QStringLiteral("stable"))
            || (filter == QStringLiteral("beta") && normalizedChannel == QStringLiteral("beta"))
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
    const QString sdkBin = provider == QStringLiteral("node")
        ? QFileInfo(executable).absolutePath()
        : QFileInfo(executable).absolutePath();
    environment.insert(QStringLiteral("PATH"),
                       QDir::toNativeSeparators(sdkBin) + u';'
                           + environment.value(QStringLiteral("PATH")));
    process.setProcessEnvironment(environment);

    if (provider == QStringLiteral("node")) {
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
           "  svm list [filter] [provider]\n"
           "  svm <provider> [arguments...]\n\n"
           "Examples:\n"
           "  svm use node 24.18.0\n"
           "  svm list lts node\n"
           "  svm node --version\n"
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
    if (command == QStringLiteral("list"))
        return commandList(arguments);
    if (command == QStringLiteral("node") || command == QStringLiteral("flutter"))
        return proxyCommand(command, arguments);

    err << "Unknown command or provider: " << command << Qt::endl;
    printHelp();
    return 2;
}
