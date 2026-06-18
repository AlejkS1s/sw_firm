# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.10...3.31)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(python /mnt/Data2/esp-2020/.espenv/bin/python)
set(PYTHON /mnt/Data2/esp-2020/.espenv/bin/python)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(switch_firm)
