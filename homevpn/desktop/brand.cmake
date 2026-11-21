# The name and icons of HomeVPN on Linux and Windows. Usage: cmake -C homevpn/desktop/brand.cmake ...
# Internal names (executable, service, settings, keychain) stay as upstream, so an installed client keeps its settings.
set(CLIENT_DISPLAY_NAME "ЧихНет" CACHE STRING "")
set(CLIENT_LINUX_DESKTOP_FILE "${CMAKE_CURRENT_LIST_DIR}/app.desktop" CACHE FILEPATH "")
set(CLIENT_LINUX_ICON_FILE "${CMAKE_CURRENT_LIST_DIR}/app.png" CACHE FILEPATH "")
set(CLIENT_WINDOWS_ICON_FILE "${CMAKE_CURRENT_LIST_DIR}/app.ico" CACHE FILEPATH "")
set(CLIENT_TRAY_ICONS_PREFIX ":/homevpn/tray/" CACHE STRING "")
set(CLIENT_EXTRA_QRC "${CMAKE_CURRENT_LIST_DIR}/tray.qrc" CACHE STRING "")
