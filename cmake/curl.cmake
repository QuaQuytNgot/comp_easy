cmake_minimum_required(VERSION 3.14)

include(ExternalProject)

set(somewhere1 ${CMAKE_SOURCE_DIR}/dependencies/prefix/wolfssl)
set(somewhere2 ${CMAKE_SOURCE_DIR}/dependencies/prefix/nghttp3)
set(somewhere3 ${CMAKE_SOURCE_DIR}/dependencies/prefix/ngtcp2)
set(somewhere4 ${CMAKE_SOURCE_DIR}/dependencies/prefix/curl)

ExternalProject_Add(wolfssl
    GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
    GIT_TAG v5.8.2-stable
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/dependencies/wolfssl"
    CONFIGURE_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/wolfssl && autoreconf -fi && ./configure --enable-quic --enable-session-ticket --enable-earlydata --enable-psk --enable-harden --enable-altcertchains --prefix=${somewhere1}
    BUILD_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/wolfssl && make
    INSTALL_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/wolfssl && make install
)

ExternalProject_Add(nghttp3
    GIT_REPOSITORY https://github.com/ngtcp2/nghttp3.git
    GIT_TAG v1.11.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/dependencies/nghttp3"
    CONFIGURE_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/nghttp3 && git submodule update --init && autoreconf -fi && ./configure --prefix=${somewhere2} --enable-lib-only
    BUILD_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/nghttp3 && make
    INSTALL_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/nghttp3 && make install
)

ExternalProject_Add(ngtcp2
    DEPENDS wolfssl nghttp3
    GIT_REPOSITORY https://github.com/ngtcp2/ngtcp2.git
    GIT_TAG v1.6.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/dependencies/ngtcp2"
    CONFIGURE_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/ngtcp2 && autoreconf -fi && ./configure PKG_CONFIG_PATH=${somewhere1}/lib/pkgconfig:${somewhere2}/lib/pkgconfig LDFLAGS=-Wl,-rpath,${somewhere1}/lib --prefix=${somewhere3} --enable-lib-only --with-wolfssl
    BUILD_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/ngtcp2 && make
    INSTALL_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/ngtcp2 && make install
)

ExternalProject_Add(curl
    DEPENDS ngtcp2
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_15_0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/dependencies/curl"
    CONFIGURE_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/curl && autoreconf -fi && ./configure --with-wolfssl=${somewhere1} --with-nghttp3=${somewhere2} --with-ngtcp2=${somewhere3} --prefix=${somewhere4}
    BUILD_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/curl && make
    INSTALL_COMMAND cd ${CMAKE_SOURCE_DIR}/dependencies/curl && make install
)