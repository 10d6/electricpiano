cmake_minimum_required(VERSION 3.22)
project(electricpiano LANGUAGES C CXX)   # C (and OBJC/OBJCXX on macOS) for the plugin build
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

add_subdirectory(synthkit)
include(CTest)

synthkit_add_synth(electricpiano SOURCES src/Synth.cpp)
