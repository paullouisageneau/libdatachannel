#[=======================================================================[.rst
FindUsrsctp
----------

FindModule for Usrsctp library

Imported Targets
^^^^^^^^^^^^^^^^

This module defines the :prop_tgt:`IMPORTED` target ``Usrsctp::Usrsctp``.

Result Variables
^^^^^^^^^^^^^^^^

This module sets the following variables:

``Usrsctp_FOUND``
  True, if the library was found.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_path(
  Usrsctp_INCLUDE_DIR
  NAMES usrsctp.h
  PATHS /usr/include /usr/local/include)

find_library(
Usrsctp_LIBRARY
  NAMES usrsctp libusrsctp
  PATHS /usr/lib /usr/local/lib)

find_package_handle_standard_args(
  Usrsctp
  REQUIRED_VARS Usrsctp_LIBRARY Usrsctp_INCLUDE_DIR)
mark_as_advanced(Usrsctp_INCLUDE_DIR Usrsctp_LIBRARY)

if(Usrsctp_FOUND)
  if(NOT TARGET Usrsctp::Usrsctp)
    if(IS_ABSOLUTE "${Usrsctp_LIBRARY}")
      add_library(Usrsctp::Usrsctp UNKNOWN IMPORTED)
      set_property(TARGET Usrsctp::Usrsctp PROPERTY IMPORTED_LOCATION "${Usrsctp_LIBRARY}")
    else()
      add_library(Usrsctp::Usrsctp INTERFACE IMPORTED)
      set_property(TARGET Usrsctp::Usrsctp PROPERTY IMPORTED_LIBNAME "${Usrsctp_LIBRARY}")
    endif()

    set_target_properties(Usrsctp::Usrsctp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Usrsctp_INCLUDE_DIR}")
  endif()
endif()
