#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "NetSdk::net_sdk" for configuration "Release"
set_property(TARGET NetSdk::net_sdk APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(NetSdk::net_sdk PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/net_sdk.lib"
  )

list(APPEND _cmake_import_check_targets NetSdk::net_sdk )
list(APPEND _cmake_import_check_files_for_NetSdk::net_sdk "${_IMPORT_PREFIX}/lib/net_sdk.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
