# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_version(<source dir> <version var> <package version var> <inputs var>)
#
# The version is the highest valid tag v<major>.<minor>.<patch>[suffix] merged into HEAD, `unknown`
# (package 0.0.0) where there is no git of the source dir's own or no such tag. The inputs are the
# files whose change can move that answer: the loose tags, the packed refs, HEAD and its branch.
function(kickos_version source version_var package_var inputs_var)
  set(version "unknown")
  set(package "0.0.0")
  set(inputs "")
  execute_process(
    COMMAND git -C "${source}" rev-parse --show-toplevel
    OUTPUT_VARIABLE top OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE top_rc)
  set(own_git FALSE)
  if(top_rc EQUAL 0)
    file(REAL_PATH "${top}" top)
    file(REAL_PATH "${source}" source_real)
    if(top STREQUAL source_real)
      set(own_git TRUE)
    endif()
  endif()
  if(own_git)
    execute_process(
      COMMAND git -C "${source}" tag --list "v*" --merged HEAD --sort=-v:refname
      OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE tags_rc)
    if(tags_rc EQUAL 0)
      # A line holding `;` would split as a list; no valid tag holds one.
      string(REGEX REPLACE "[^\n]*;[^\n]*" "" tags "${tags}")
      string(REPLACE "\n" ";" tags "${tags}")
      foreach(tag IN LISTS tags)
        if(tag MATCHES "^v(([0-9]+)[.]([0-9]+)[.]([0-9]+)[-+.A-Za-z0-9]*)$")
          set(version "${CMAKE_MATCH_1}")
          set(package "${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.${CMAKE_MATCH_4}")
          break()
        endif()
      endforeach()
    endif()
    execute_process(
      COMMAND git -C "${source}" rev-parse --path-format=absolute --git-common-dir --git-dir
      OUTPUT_VARIABLE dirs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE dirs_rc)
    execute_process(
      COMMAND git -C "${source}" symbolic-ref -q HEAD
      OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(dirs_rc EQUAL 0)
      string(REPLACE "\n" ";" dirs "${dirs}")
      list(GET dirs 0 common)
      list(GET dirs 1 own)
      set(refs "${common}/refs/tags" "${common}/packed-refs" "${own}/HEAD")
      # HEAD names its branch; the branch moving moves the tags HEAD has merged.
      if(NOT branch STREQUAL "")
        list(APPEND refs "${common}/${branch}")
      endif()
      foreach(ref IN LISTS refs)
        if(EXISTS "${ref}")
          list(APPEND inputs "${ref}")
        endif()
      endforeach()
    endif()
  endif()
  file(STRINGS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../user/include/kickos/sys/banner_identity.h"
       max REGEX "BANNER_IDENTITY_VERSION_MAX = [0-9]+")
  string(REGEX REPLACE ".*= ([0-9]+).*" "\\1" max "${max}")
  string(LENGTH "${version}" len)
  if(len GREATER max)
    message(FATAL_ERROR "version ${version} is ${len} characters, past the banner's "
                        "${max}-character BANNER_IDENTITY_VERSION_MAX")
  endif()
  set(${version_var} "${version}" PARENT_SCOPE)
  set(${package_var} "${package}" PARENT_SCOPE)
  set(${inputs_var} "${inputs}" PARENT_SCOPE)
endfunction()
