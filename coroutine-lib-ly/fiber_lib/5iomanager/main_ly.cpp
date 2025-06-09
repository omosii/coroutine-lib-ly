
#include "ioscheduler_ly.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <cerrno>

using namespace sylar;

char recv_data[4096];

const char data[] = "GET / HTTP/1.0\r\n\r\n";

int sock;

void func()
{
    recv(sock, recv_data, 4096, 0);
    std::cout << recv_data << std::endl << std::endl;
}

void func2()
{
    send(sock, data, sizeof(data), 0);
}

int main(int argc, char const *argv[])
{
    // 一个主线程 + 一个调度线程 ，主线程参与调度
    IOManager manager(2);
    // IPv4 TCP socket    // 0 -> 自动选择协议
    sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(80);  // HTTP 标准端口
    server.sin_addr.s_addr = inet_addr("103.235.46.96"); // IP 地址

    fcntl(sock, F_SETFL, O_NONBLOCK); // 设置为非阻塞模式

    connect(sock, (struct sockaddr *)&server, sizeof(server));

    // 添加写事件和读事件
    manager.addEvent(sock, IOManager::WRITE, &func2);
    manager.addEvent(sock, IOManager::READ, &func);

    std::cout << "event has been posted\n\n";

    return 0;
}








