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

configure_file(LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/bin/LICENSE" COPYONLY)
configure_file(THIRD_PARTY_LICENSES.txt "${CMAKE_CURRENT_SOURCE_DIR}/bin/THIRD_PARTY_LICENSES.txt" COPYONLY)

add_library(mfmt_lib STATIC source/format.c source/config.c)
target_include_directories(mfmt_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mfmt_lib PRIVATE mc_syntax mvm_toml)

add_executable(mvm-fmt source/main.c)
target_link_libraries(mvm-fmt PRIVATE mfmt_lib)
set_target_properties(mvm-fmt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>")

foreach(target IN ITEMS mfmt_lib mvm-fmt)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
