#[=======================================================================[.rst
Findplog
----------

FindModule for Plog library

Imported Targets
^^^^^^^^^^^^^^^^

This module defines the :prop_tgt:`IMPORTED` target ``plog::plog``.

Result Variables
^^^^^^^^^^^^^^^^

This module sets the following variables:

``plog_FOUND``
  True, if the library was found.

Cache variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``plog_INCLUDE_DIR``
  Directory containing ``plog/Log.h``.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_path(
  plog_INCLUDE_DIR
  NAMES plog/Log.h
  PATHS /usr/include /usr/local/include)

find_package_handle_standard_args(
  plog
  REQUIRED_VARS plog_INCLUDE_DIR)
mark_as_advanced(plog_INCLUDE_DIR)

if(plog_FOUND)
  if(NOT TARGET plog::plog)
    add_library(plog::plog INTERFACE IMPORTED)
    set_target_properties(plog::plog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${plog_INCLUDE_DIR}")
  endif()
endif()
