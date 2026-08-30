foreach(required_variable IN ITEMS
        TEST_ROOT
        MNN_ARCHIVE
        SHARED_FIXTURE
        AR_EXECUTABLE
        VALIDATOR_MODULE
        VALIDATION_ENTRYPOINT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/fake-prefix/lib")
set(fake_archive "${TEST_ROOT}/fake-prefix/lib/libMNN.a")
configure_file("${SHARED_FIXTURE}" "${fake_archive}" COPYONLY)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DARCHIVE_PATH=${MNN_ARCHIVE}"
        "-DAR_EXECUTABLE=${AR_EXECUTABLE}"
        "-DVALIDATOR_MODULE=${VALIDATOR_MODULE}"
        -P "${VALIDATION_ENTRYPOINT}"
    RESULT_VARIABLE valid_status
    OUTPUT_VARIABLE valid_stdout
    ERROR_VARIABLE valid_stderr)
if(NOT valid_status EQUAL 0)
    message(FATAL_ERROR
        "The pinned MNN archive was rejected (${valid_status})\n"
        "${valid_stdout}\n${valid_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DARCHIVE_PATH=${fake_archive}"
        "-DAR_EXECUTABLE=${AR_EXECUTABLE}"
        "-DVALIDATOR_MODULE=${VALIDATOR_MODULE}"
        -P "${VALIDATION_ENTRYPOINT}"
    RESULT_VARIABLE fake_status
    OUTPUT_VARIABLE fake_stdout
    ERROR_VARIABLE fake_stderr)
if(fake_status EQUAL 0)
    message(FATAL_ERROR
        "A shared object disguised as libMNN.a passed static archive validation")
endif()
set(fake_output "${fake_stdout}\n${fake_stderr}")
if(NOT fake_output MATCHES "not a regular static archive")
    message(FATAL_ERROR
        "The disguised shared object failed for an unexpected reason\n${fake_output}")
endif()
