/**
 * Qt signaling server example for libdatachannel
 * Copyright (c) 2022 cheungxiongwei
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "SignalingServer.h"
#include <QCoreApplication>

int main(int argc, char *argv[]) {
	QCoreApplication a(argc, argv);
	SignalingServer server;
	if (server.listen(QHostAddress("127.0.0.1"), 8000)) {
		qInfo() << "Listening on 127.0.0.1:8000";
		qInfo() << "server listening on 127.0.0.1:8000";
	} else {
		qDebug() << "[Errno 10000] error while attempting to bind on address ('127.0.0.1', 8000)";
	}
	return a.exec();
}
