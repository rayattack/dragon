# Runs once per generator: keep /usr for .deb/.rpm, clear it for the tarball
# so bin/lib/share/include sit at the archive root like the macOS tarball.
if(CPACK_GENERATOR STREQUAL "TGZ")
  set(CPACK_PACKAGING_INSTALL_PREFIX "")
endif()
