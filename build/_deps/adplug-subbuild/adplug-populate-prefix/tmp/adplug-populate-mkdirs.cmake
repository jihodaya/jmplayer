# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src")
  file(MAKE_DIRECTORY "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src")
endif()
file(MAKE_DIRECTORY
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-build"
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix"
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/tmp"
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/src/adplug-populate-stamp"
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/src"
  "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/src/adplug-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/src/adplug-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-subbuild/adplug-populate-prefix/src/adplug-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
