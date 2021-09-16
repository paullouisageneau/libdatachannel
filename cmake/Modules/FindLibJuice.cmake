if (NOT TARGET LibJuice::LibJuice)
	find_path(JUICE_INCLUDE_DIR juice/juice.h)
	find_library(JUICE_LIBRARY NAMES juice)

	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(LibJuice DEFAULT_MSG JUICE_LIBRARY JUICE_INCLUDE_DIR)

    if (LibJuice_FOUND)
        add_library(LibJuice::LibJuice UNKNOWN IMPORTED)
        set_target_properties(LibJuice::LibJuice PROPERTIES
            IMPORTED_LOCATION "${JUICE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${JUICE_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${JUICE_LIBRARIES}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    endif ()
endif ()

