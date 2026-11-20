cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The project name is the app description's project_name, which OTA compares
# before it installs an image: the board and its flash layout (bump the l<N>
# whenever partitions-16mb.csv changes; layout 3: 4 MB app slots).
project(z1fw-z1-l3)
