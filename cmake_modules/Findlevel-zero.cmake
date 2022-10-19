if(LEVEL_ZERO_ROOT_DIR)
    message(STATUS "Trying to locate level-zero library and headers under ${LEVEL_ZERO_ROOT_DIR}")
    find_library(ZE_LOADER_LIBRARY "ze_loader" HINTS "${LEVEL_ZERO_ROOT_DIR}/lib" "${LEVEL_ZERO_ROOT_DIR}/lib64" NO_DEFAULT_PATH)
    find_path (LEVEL_ZERO_INCLUDE_DIR NAMES "level_zero/ze_api.h" PATHS "${LEVEL_ZERO_ROOT_DIR}/include" NO_DEFAULT_PATH)

    if(ZE_LOADER_LIBRARY AND LEVEL_ZERO_INCLUDE_DIR)
	    get_filename_component(ZE_LOADER_LIBRARY_DIR ${ZE_LOADER_LIBRARY} DIRECTORY)
	    check_library_exists("ze_loader" "zeInit" ${ZE_LOADER_LIBRARY_DIR} ZE_LOADER_HAVE_ZEINIT)

        if(ZE_LOADER_HAVE_ZEINIT)
            message(STATUS "Found ze_loader library in ${ZE_LOADER_LIBRARY} and level_zero/ze_api.h in ${LEVEL_ZERO_INCLUDE_DIR}")
            add_library(level_zero::ze_loader UNKNOWN IMPORTED GLOBAL)
            set_property(TARGET level_zero::ze_loader PROPERTY IMPORTED_LOCATION "${ZE_LOADER_LIBRARY}")
            set_property(TARGET level_zero::ze_loader PROPERTY INTERFACE "${LEVEL_ZERO_INCLUDE_DIR}/level_zero/ze_api.h")
            include_directories("${LEVEL_ZERO_INCLUDE_DIR}/") 
            set(LEVEL_ZERO_FOUND TRUE)
        else(ZE_LOADER_HAVE_ZEINIT)
            if(NOT ZE_LOADER_HAVE_ZEINIT)
                message(WARNING "Found ze_loader library under ${ZE_LOADER_LIBRARY}, but could not find symbol zeInit in this library -- falling back to package config search")
            endif(NOT ZE_LOADER_HAVE_ZEINIT)
        endif(ZE_LOADER_HAVE_ZEINIT)
    else(ZE_LOADER_LIBRARY AND LEVEL_ZERO_INCLUDE_DIR)
	    if(NOT ZE_LOADER_LIBRARY)
		    message(WARNING "Could not find ze_loader library under provided LEVEL_ZERO_ROOT_DIR='${LEVEL_ZERO_ROOT_DIR}' (tried subdirectories lib/ and lib64/) -- falling back to package config search")
	    endif(NOT ZE_LOADER_LIBRARY)
	    if(NOT LEVEL_ZERO_INCLUDE_DIR)
		    message(WARNING "Cound not find level_zero/ze_api.h under provided LEVEL_ZERO_ROOT_DIR=${LEVEL_ZERO_ROOT_DIR}' (tried subdirectory include) -- falling back to package config search")
	    endif(NOT LEVEL_ZERO_INCLUDE_DIR)
    endif(ZE_LOADER_LIBRARY AND LEVEL_ZERO_INCLUDE_DIR)
endif(LEVEL_ZERO_ROOT_DIR)

if(NOT LEVEL_ZERO_FOUND)
    find_package(PkgConfig QUIET)

    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LEVEL_ZERO level-zero)
        if(LEVEL_ZERO_FOUND)
            pkg_get_variable(LEVEL_ZERO_LIBRARY_DIR level-zero libdir)
            pkg_get_variable(LEVEL_ZERO_INCLUDE_DIR level-zero includedir)
            add_library(level_zero::ze_loader UNKNOWN IMPORTED GLOBAL)
            set_property(TARGET level_zero::ze_loader PROPERTY IMPORTED_LOCATION "${LEVEL_ZERO_LIBRARY_DIR}/libze_loader.so")
            set_property(TARGET level_zero::ze_loader PROPERTY INTERFACE "${LEVEL_ZERO_INCLUDE_DIR}/level_zero/ze_api.h")
            include_directories("${LEVEL_ZERO_INCLUDE_DIR}/")
        endif(LEVEL_ZERO_FOUND)
    endif(PKG_CONFIG_FOUND)
endif(NOT LEVEL_ZERO_FOUND)
