foreach(required_variable IN ITEMS BINARY_PATH READELF_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${READELF_EXECUTABLE}" -d "${BINARY_PATH}"
    RESULT_VARIABLE readelf_status
    OUTPUT_VARIABLE dynamic_section
    ERROR_VARIABLE readelf_stderr)
if(NOT readelf_status EQUAL 0)
    message(FATAL_ERROR
        "readelf failed for ${BINARY_PATH} (${readelf_status})\n${readelf_stderr}")
endif()

string(REGEX MATCH
    "NEEDED[^\r\n]*\\[libMNN[^]]*\\.so[^]]*\\]"
    mnn_shared_dependency
    "${dynamic_section}")
if(mnn_shared_dependency)
    message(FATAL_ERROR
        "Static MNN deployment contract violated by ${BINARY_PATH}: "
        "${mnn_shared_dependency}")
endif()
