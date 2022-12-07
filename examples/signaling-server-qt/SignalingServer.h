/*
 * Qt signaling server example for libdatachannel
 * Copyright (c) 2022 cheungxiongwei
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef SIGNALINGSERVER_H
#define SIGNALINGSERVER_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QWebSocketServer;
class QWebSocket;
class SignalingServer : public QObject {
	Q_OBJECT
public:
	explicit SignalingServer(QObject *parent = nullptr);
	bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0);
private slots:
	void onNewConnection();
	void onDisconnected();
	void onWebSocketError(QAbstractSocket::SocketError error);
	void onBinaryMessageReceived(const QByteArray &message);
	void onTextMessageReceived(const QString &message);

private:
	QWebSocketServer *server;
	QMap<QString, QWebSocket *> clients;
};

#endif // SIGNALINGSERVER_H
