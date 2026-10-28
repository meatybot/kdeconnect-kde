pkg_check_modules(PulseSimple REQUIRED IMPORTED_TARGET libpulse-simple)

# Everything but the device-specific parts, shared with the tests
add_library(kdeconnect_audiostream_static STATIC
    audioframe.cpp
    audiostreamprotocol.cpp
    audiostreamsession.cpp
    framequeue.cpp
    jitterbuffer.cpp
)
set_target_properties(kdeconnect_audiostream_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(kdeconnect_audiostream_static PUBLIC kdeconnectcore Qt::Core)

kdeconnect_add_plugin(kdeconnect_audiostream SOURCES audiostreamplugin.cpp pulsecapture.cpp)

target_link_libraries(kdeconnect_audiostream
    kdeconnect_audiostream_static
    kdeconnectcore
    Qt::DBus
    Qt::Multimedia
    KF6::I18n
    KF6::Notifications
    PkgConfig::PulseSimple
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
