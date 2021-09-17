/*
xp_pen_userland
Copyright (C) 2021 - Aren Villanueva <https://github.com/kurikaesu/>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#if __has_include(<filesystem>)
#include <filesystem>
namespace filesystem = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
  namespace filesystem = std::experimental::filesystem;
#else
  error "Missing the <filesystem> header."
#endif

#include <sys/socket.h>
#include <sys/un.h>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include "socket_server.h"
#include "unix_socket_message.h"

socket_server::socket_server() {
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    enabled = sock != -1;

    if (!enabled) {
        std::cout << "Could not create socket" << std::endl;
        return;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    enabled = fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;

    if (!enabled) {
        std::cout << "Could not set non-blocking operation on socket" << std::endl;
        close(sock);
        return;
    }

    std::stringstream  socketLocationDir;
    socketLocationDir << getenv("HOME");
    socketLocationDir << "/.local/var/run/";

    filesystem::create_directories(socketLocationDir.str());
    std::stringstream  socketLocation;
    socketLocation << socketLocationDir.str();
    socketLocation << "xp_pen_userland.sock";

    enabled = remove(socketLocation.str().c_str()) != -1;

    if (!enabled && errno != ENOENT) {
        std::cout << "Could not set socket location to " << socketLocation.str() << " errno: " << errno << std::endl;
        close(sock);
        return;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socketLocation.str().c_str(), sizeof(address.sun_path) - 1);

    enabled = bind(sock, (struct sockaddr*)&address, sizeof(struct sockaddr_un)) != -1;

    if (!enabled) {
        std::cout << "Problem when binding socket " << socketLocation.str() << std::endl;
        close(sock);
        return;
    }

    listen(sock, 5);
    std::cout << "Listening on socket " << socketLocation.str() << std::endl;
}

socket_server::~socket_server() {
    for (auto socket : connectedSockets) {
        close(socket);
    }

    if (enabled) {
        close(sock);
    }

    std::stringstream  socketLocation;
    socketLocation << getenv("HOME");
    socketLocation << "/.local/var/run/xp_pen_userland.sock";

    remove(socketLocation.str().c_str());
}

void socket_server::handleConnections() {
    if (enabled) {
        while (true) {
            int newConnection = accept(sock, NULL, NULL);
            if (newConnection == -1) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    std::cout << "Error when accepting connection on socket" << std::endl;
                }

                break;
            }

            connectedSockets.push_back(newConnection);
        }
    }
}

void socket_server::handleMessages(unix_socket_message_queue* messageQueue) {
    int socketCount = connectedSockets.size();
    if (socketCount > 0) {

        struct pollfd *fds = new pollfd[socketCount];
        char headerBuffer[sizeof(unix_socket_message_header)];

        int res = poll(fds, socketCount, 0);
        if (res > 0) {
            for (int idx = 0; idx < socketCount; ++idx) {
                if (fds[idx].revents != 0) {
                    if (fds[idx].revents & POLLIN) {
                        ssize_t s = read(fds[idx].fd, headerBuffer, sizeof(headerBuffer));
                        if (s != sizeof(headerBuffer)) {
                            // Validate the signature
                            struct unix_socket_message *message = new unix_socket_message;
                            memcpy(&message, headerBuffer, sizeof(headerBuffer));

                            if (message->signature == 53784359345776669L) {
                                message->data = new char[message->length];
                                ssize_t totalRead = 0;
                                bool failed = false;

                                while (totalRead < message->length) {
                                    s = read(fds[idx].fd, message->data + totalRead, message->length - totalRead);

                                    if (s == -1) {
                                        // Handle something going wrong
                                        delete[] message->data;
                                        delete message;
                                        failed = true;
                                        break;
                                    }
                                    totalRead += s;
                                }

                                if (!failed) {
                                    messageQueue->addMessage(message);
                                }
                            }
                        }
                    } else {
                        auto record = std::find(connectedSockets.begin(), connectedSockets.end(), fds[idx].fd);
                        if (record != connectedSockets.end()) {
                            connectedSockets.erase(record);
                        }
                        close(fds[idx].fd);
                    }
                }
            }
        }

        delete[] fds;
    }
}