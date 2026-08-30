if(NOT DEFINED BUILD_DIR OR NOT DEFINED STAGE_DIR)
    message(FATAL_ERROR "BUILD_DIR and STAGE_DIR are required")
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
    RESULT_VARIABLE install_status
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)
if(NOT install_status EQUAL 0)
    message(FATAL_ERROR
        "staging install failed (${install_status})\n${install_stdout}\n${install_stderr}")
endif()

set(required_files
    "bin/eggvision_app"
    "share/doc/eggvision/THIRD_PARTY_NOTICES.md"
    "share/licenses/eggvision/MNN-LICENSE.txt"
    "share/licenses/eggvision/MNN-FLATBUFFERS-LICENSE.txt"
    "share/licenses/eggvision/MNN-HALF-LICENSE.txt"
    "share/licenses/eggvision/MNN-PROTOBUF-LICENSE.txt"
    "share/licenses/eggvision/MNN-RAPIDJSON-LICENSE.txt")
foreach(relative_path IN LISTS required_files)
    if(NOT EXISTS "${STAGE_DIR}/${relative_path}")
        message(FATAL_ERROR "staging install omitted ${relative_path}")
    endif()
endforeach()
