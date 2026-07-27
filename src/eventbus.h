#pragma once

#include <QJsonObject>
#include <QObject>

class QLocalServer;

QString svmEventBusName();
void publishSvmEvent(const QJsonObject &event);

class SvmEventBus final : public QObject
{
    Q_OBJECT

public:
    explicit SvmEventBus(QObject *parent = nullptr);
    bool start();

signals:
    void eventReceived(const QJsonObject &event);

private:
    QLocalServer *m_server = nullptr;
};
