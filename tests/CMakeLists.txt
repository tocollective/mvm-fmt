# Copyright 2026 tocollective contributors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_executable(mfmt_test format_test.c)
target_link_libraries(mfmt_test PRIVATE mfmt_lib)
set_target_properties(mfmt_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
file(GLOB_RECURSE formatter_corpus CONFIGURE_DEPENDS
    "${M_SOURCE_DIR}/mvm-c/examples/*.m"
    "${M_SOURCE_DIR}/mvm-lib/*.m"
    "${MVM_SOURCE_DIR}/benchmarks/*.m")
add_test(NAME mfmt_format COMMAND mfmt_test ${formatter_corpus})
set_tests_properties(mfmt_format PROPERTIES TIMEOUT 60)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME mfmt_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py"
            --formatter $<TARGET_FILE:mvm-fmt> --compiler $<TARGET_FILE:mvm-c>
            --source-dir "${M_SOURCE_DIR}"
            --work-dir "${CMAKE_CURRENT_BINARY_DIR}/cli")
    set_tests_properties(mfmt_cli PROPERTIES TIMEOUT 120)
endif()
