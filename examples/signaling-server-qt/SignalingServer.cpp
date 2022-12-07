/**
 * Qt signaling server example for libdatachannel
 * Copyright (c) 2022 cheungxiongwei
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "SignalingServer.h"
#include <QDebug>
#include <QtWebSockets>
#include <format>

SignalingServer::SignalingServer(QObject *parent) : QObject(parent) {
	server = new QWebSocketServer("SignalingServer", QWebSocketServer::NonSecureMode, this);
	QObject::connect(server, &QWebSocketServer::newConnection, this,
	                 &SignalingServer::onNewConnection);
}

bool SignalingServer::listen(const QHostAddress &address, quint16 port) {
	return server->listen(address, port);
}

void SignalingServer::onNewConnection() {
	auto webSocket = server->nextPendingConnection();
	auto client_id = webSocket->requestUrl().path().split("/").at(1);
	qInfo() << QString::fromStdString(
	    std::format("Client {} connected", client_id.toUtf8().constData()));

	clients[client_id] = webSocket;

	webSocket->setObjectName(client_id);
	QObject::connect(webSocket, &QWebSocket::disconnected, this, &SignalingServer::onDisconnected);
	QObject::connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
	                 this, &SignalingServer::onWebSocketError);
	QObject::connect(webSocket, &QWebSocket::binaryMessageReceived, this,
	                 &SignalingServer::onBinaryMessageReceived);
	QObject::connect(webSocket, &QWebSocket::textMessageReceived, this,
	                 &SignalingServer::onTextMessageReceived);
}

void SignalingServer::onDisconnected() {
	QWebSocket *webSocket = qobject_cast<QWebSocket *>(sender());
	clients.remove(webSocket->objectName());
}

void SignalingServer::onWebSocketError(QAbstractSocket::SocketError error) {
	qDebug() << QString::fromStdString(std::format("Client {} << {}",
	                                               sender()->objectName().toUtf8().constData(),
	                                               QString::number(error).toUtf8().constData()));
}

void SignalingServer::onBinaryMessageReceived(const QByteArray &message) {
	qInfo() << QString::fromStdString(std::format(
	    "Client {} << {}", sender()->objectName().toUtf8().constData(), message.constData()));
}

void SignalingServer::onTextMessageReceived(const QString &message) {
	QWebSocket *webSocket = qobject_cast<QWebSocket *>(sender());

	qInfo() << QString::fromStdString(std::format("Client {} << {}",
	                                              webSocket->objectName().toUtf8().constData(),
	                                              message.toUtf8().constData()));

	auto JsonObject = QJsonDocument::fromJson(message.toUtf8()).object();
	auto destination_id = JsonObject["id"].toString();
	auto destination_websocket = clients[destination_id];
	if (destination_websocket) {
		JsonObject["id"] = webSocket->objectName();
		auto data = QJsonDocument(JsonObject).toJson(QJsonDocument::Compact);
		qInfo() << QString::fromStdString(
		    std::format("Client {} >> {}", destination_id.toUtf8().constData(), data.constData()));
		destination_websocket->sendTextMessage(QString(data));
		destination_websocket->flush();
	} else {
		qInfo() << QString::fromStdString(
		    std::format("Client {} not found", destination_id.toUtf8().constData()));
	}
}
