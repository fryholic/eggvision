function(eggvision_validate_static_archive archive_path archive_label)
    if(NOT EXISTS "${archive_path}" OR IS_DIRECTORY "${archive_path}")
        message(FATAL_ERROR "${archive_label} archive not found: ${archive_path}")
    endif()
    if(NOT CMAKE_AR OR CMAKE_AR MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "An archiver is required to validate ${archive_label}: ${archive_path}")
    endif()

    file(READ "${archive_path}" archive_magic OFFSET 0 LIMIT 8 HEX)
    string(TOLOWER "${archive_magic}" archive_magic)
    if(NOT archive_magic STREQUAL "213c617263683e0a")
        message(FATAL_ERROR
            "${archive_label} is not a regular static archive: ${archive_path}")
    endif()

    execute_process(
        COMMAND "${CMAKE_AR}" -t "${archive_path}"
        RESULT_VARIABLE archive_status
        OUTPUT_QUIET
        ERROR_VARIABLE archive_stderr)
    if(NOT archive_status EQUAL 0)
        string(STRIP "${archive_stderr}" archive_stderr)
        message(FATAL_ERROR
            "${archive_label} cannot be read by ${CMAKE_AR}: ${archive_path}\n"
            "${archive_stderr}")
    endif()
endfunction()
