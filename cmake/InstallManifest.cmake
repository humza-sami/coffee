# Runs last during `cmake --install`: records "<sha256>  <relative path>" for every installed file.
set(_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
set(_out "# coffee install manifest v1\n# version ${COFFEE_VERSION_FULL}\n")
foreach(_rel IN LISTS COFFEE_INSTALL_FILES)
  file(SHA256 "${_root}/${_rel}" _h)
  string(APPEND _out "${_h}  ${_rel}\n")
endforeach()
# InstallGuard.cmake already established that this path is absent or our own regular file (never a symlink).
file(WRITE "${_root}/share/coffee/install-manifest.txt" "${_out}")
message(STATUS "coffee: manifest written to ${_root}/share/coffee/install-manifest.txt (remove with share/coffee/uninstall.sh --prefix ${_root})")
