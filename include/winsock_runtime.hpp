#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <stdexcept>
#include <string>



class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};

        int result =
            WSAStartup(
                MAKEWORD(2, 2),
                &data
            );

        if (result != 0) {
            throw std::runtime_error(
                "WSAStartup failed: " +
                std::to_string(result)
            );
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }

    WinsockRuntime(
        const WinsockRuntime&
    ) = delete;

    WinsockRuntime& operator=(
        const WinsockRuntime&
    ) = delete;
};