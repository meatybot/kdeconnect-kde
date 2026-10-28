find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

ecm_add_test(audiostreamcodectest.cpp LINK_LIBRARIES kdeconnect_audiostream_static Qt::Test)
