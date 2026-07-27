#include "eventbus.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

QString svmEventBusName()
{
    const QByteArray identity =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toUtf8();
    const QByteArray suffix =
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(16);
    return QStringLiteral("svm-eventbus-") + QString::fromLatin1(suffix);
}

void publishSvmEvent(const QJsonObject &event)
{
    QLocalSocket socket;
    socket.connectToServer(svmEventBusName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(100))
        return;
    socket.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    socket.flush();
    socket.waitForBytesWritten(100);
    socket.disconnectFromServer();
}

SvmEventBus::SvmEventBus(QObject *parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
{
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            auto *payload = new QByteArray;
            connect(socket, &QLocalSocket::readyRead, socket, [socket, payload] {
                payload->append(socket->readAll());
            });
            connect(socket, &QLocalSocket::disconnected, socket, [this, socket, payload] {
                payload->append(socket->readAll());
                const QJsonObject event = QJsonDocument::fromJson(*payload).object();
                delete payload;
                if (!event.isEmpty())
                    emit eventReceived(event);
                socket->deleteLater();
            });
        }
    });
}

bool SvmEventBus::start()
{
    if (m_server->isListening())
        return true;
    if (m_server->listen(svmEventBusName()))
        return true;
    QLocalServer::removeServer(svmEventBusName());
    return m_server->listen(svmEventBusName());
}
