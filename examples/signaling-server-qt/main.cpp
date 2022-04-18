#include <QCoreApplication>
#include "SignalingServer.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    SignalingServer server;
    if(server.listen(QHostAddress("127.0.0.1"),8000)){
        qInfo() << "Listening on 127.0.0.1:8000";
        qInfo() << "server listening on 127.0.0.1:8000";
    }else{
        qDebug() << "[Errno 10000] error while attempting to bind on address ('127.0.0.1', 8000)";
    }
    return a.exec();
}
