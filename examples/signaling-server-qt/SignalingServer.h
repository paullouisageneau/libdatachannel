#ifndef SIGNALINGSERVER_H
#define SIGNALINGSERVER_H

#include <QObject>
#include <QtWebSockets>
#include <QDebug>
#include <format>

class SignalingServer : public QObject
{
    Q_OBJECT
public:
    explicit SignalingServer(QObject *parent = nullptr):QObject(parent) {
        server = new QWebSocketServer("SignalingServer",QWebSocketServer::NonSecureMode);
        QObject::connect(server,&QWebSocketServer::newConnection,this,&SignalingServer::onNewConnection);
    }
    bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0) {
        return server->listen(address,port);
    }
private slots:
    void onNewConnection() {
        auto webSocket = server->nextPendingConnection();
        auto client_id = webSocket->requestUrl().path().split("/").at(1);
        qInfo() <<  QString::fromStdString(std::format("Client {} connected",client_id.toUtf8().constData()));

        clients[client_id] = webSocket;

        webSocket->setObjectName(client_id);
        QObject::connect(webSocket,&QWebSocket::disconnected,this,&SignalingServer::onDisconnected);
        QObject::connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),this,&SignalingServer::onWebSocketError);
        QObject::connect(webSocket,&QWebSocket::binaryMessageReceived,this,&SignalingServer::onBinaryMessageReceived);
        QObject::connect(webSocket,&QWebSocket::textMessageReceived,this,&SignalingServer::onTextMessageReceived);
    }
    void onDisconnected(){
        QWebSocket* webSocket = qobject_cast<QWebSocket*>(sender());
        clients.remove(webSocket->objectName());
    }
    void onWebSocketError(QAbstractSocket::SocketError error){
        qDebug() << QString::fromStdString(std::format("Client {} << {}",sender()->objectName().toUtf8().constData(),QString::number(error).toUtf8().constData()));
    }
    void onBinaryMessageReceived(const QByteArray &message){
        qInfo() <<  QString::fromStdString(std::format("Client {} << {}",sender()->objectName().toUtf8().constData(),message.constData()));
    }
    void onTextMessageReceived(const QString &message){
        QWebSocket* webSocket = qobject_cast<QWebSocket*>(sender());

        qInfo() <<  QString::fromStdString(std::format("Client {} << {}",webSocket->objectName().toUtf8().constData(),message.toUtf8().constData()));

        auto JsonObject = QJsonDocument::fromJson(message.toUtf8()).object();
        auto destination_id = JsonObject["id"].toString();
        auto destination_websocket = clients[destination_id];
        if(destination_websocket) {
            JsonObject["id"] = webSocket->objectName();
            auto data = QJsonDocument(JsonObject).toJson(QJsonDocument::Compact);
            qInfo() <<  QString::fromStdString(std::format("Client {} >> {}",destination_id.toUtf8().constData(),data.constData()));
            destination_websocket->sendTextMessage(QString(data));
            destination_websocket->flush();
        }else{
            qInfo() <<  QString::fromStdString(std::format("Client {} not found",destination_id.toUtf8().constData()));
        }
    }
private:
    QWebSocketServer *server;
    QMap<QString,QWebSocket*> clients;
};

#endif // SIGNALINGSERVER_H
