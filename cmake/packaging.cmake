# CPack packaging configuration for qSnapper
# RPM and DEB package generation

set(CPACK_PACKAGE_NAME "qsnapper")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt6/QML GUI for Btrfs/Snapper snapshot management")
set(CPACK_PACKAGE_DESCRIPTION "A modern Qt6/QML graphical user interface for managing Btrfs filesystem snapshots via Snapper. Supports creating, browsing, comparing, and restoring snapshots with Light/Dark theme and multi-language support.")
set(CPACK_PACKAGE_VENDOR "Presire")
set(CPACK_PACKAGE_CONTACT "Presire")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.md")

# RPM settings
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "System/Monitoring")
set(CPACK_RPM_PACKAGE_URL "https://github.com/presire/qSnapper")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_REQUIRES "snapper")
set(CPACK_RPM_PACKAGE_AUTOPROV ON)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_APPEND
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/128x128
    /usr/share/icons/hicolor/128x128/apps
    /usr/share/dbus-1
    /usr/share/dbus-1/system-services
    /usr/share/dbus-1/system.d
    /usr/share/polkit-1
    /usr/share/polkit-1/actions
)

# DEB settings
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/presire/qSnapper")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "snapper")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

include(CPack)
