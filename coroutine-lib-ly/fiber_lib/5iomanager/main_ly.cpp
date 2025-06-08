
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













