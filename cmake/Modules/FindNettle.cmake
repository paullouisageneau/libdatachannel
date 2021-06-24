if (NOT TARGET Nettle::Nettle)
	find_path(NETTLE_INCLUDE_DIR nettle/hmac.h)
	find_library(NETTLE_LIBRARY NAMES nettle libnettle)

	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(Nettle DEFAULT_MSG NETTLE_LIBRARY NETTLE_INCLUDE_DIR)

    if (Nettle_FOUND)
        add_library(Nettle::Nettle UNKNOWN IMPORTED)
        set_target_properties(Nettle::Nettle PROPERTIES
            IMPORTED_LOCATION "${NETTLE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NETTLE_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${NETTLE_LIBRARIES}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    endif ()
endif ()

