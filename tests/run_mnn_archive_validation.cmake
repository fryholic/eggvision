foreach(required_variable IN ITEMS ARCHIVE_PATH AR_EXECUTABLE VALIDATOR_MODULE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(CMAKE_AR "${AR_EXECUTABLE}")
include("${VALIDATOR_MODULE}")
eggvision_validate_static_archive("${ARCHIVE_PATH}" "MNN validation fixture")
