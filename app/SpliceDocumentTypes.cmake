# Puts the document types into the bundle's Info.plist.
#
# Run by the xpcog-bundle-plist target in app/CMakeLists.txt, with
#
#   PLIST     the bundle's Contents/Info.plist
#   FRAGMENT  what xpcog-doctypes wrote
#
# CMake writes Info.plist while generating the build, from app/Info.plist.in,
# and the fragment exists only once xpcog-doctypes has been built and run -- so
# the two meet here, after the build, rather than in configure_file(). The plist
# carries a pair of marker comments and whatever lies between them is replaced,
# which is what makes this safe to run again and again: a reconfigure writes a
# fresh plist with an empty pair, a codec change writes a fresh fragment into a
# plist that still holds the old one, and either way the next build lands the
# current list. It runs on every build rather than only after a link because a
# reconfigure rewrites the plist without relinking anything.
#
# It ends by asking plutil, so a fragment that is not well-formed fails the
# build here rather than at first launch, and so a plist that somehow lost its
# document types is noticed by the build and not by a listener double-clicking
# a file.

foreach(_var PLIST FRAGMENT)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "SpliceDocumentTypes: ${_var} is not set")
    endif()
endforeach()

file(READ "${PLIST}" _plist)
file(READ "${FRAGMENT}" _fragment)

set(_begin "<!-- XPCOG_DOCUMENT_TYPES_BEGIN -->")
set(_end   "<!-- XPCOG_DOCUMENT_TYPES_END -->")

string(FIND "${_plist}" "${_begin}" _begin_at)
string(FIND "${_plist}" "${_end}"   _end_at)
if(_begin_at EQUAL -1 OR _end_at EQUAL -1 OR _end_at LESS _begin_at)
    message(FATAL_ERROR
        "SpliceDocumentTypes: ${PLIST} has no XPCOG_DOCUMENT_TYPES markers. "
        "app/Info.plist.in must carry both, BEGIN before END.")
endif()

string(LENGTH "${_begin}" _begin_length)
math(EXPR _cut "${_begin_at} + ${_begin_length}")
string(SUBSTRING "${_plist}" 0 ${_cut} _head)
string(SUBSTRING "${_plist}" ${_end_at} -1 _tail)

# The fragment's lines are tab-indented and newline-terminated; the tab before
# the tail puts the END marker back at the depth Info.plist.in gave it.
set(_spliced "${_head}\n${_fragment}\t${_tail}")

if(NOT _spliced STREQUAL _plist)
    file(WRITE "${PLIST}" "${_spliced}")
endif()

execute_process(
    COMMAND plutil -lint "${PLIST}"
    RESULT_VARIABLE _lint
    OUTPUT_VARIABLE _lint_output
    ERROR_VARIABLE  _lint_output)
if(NOT _lint EQUAL 0)
    message(FATAL_ERROR "SpliceDocumentTypes: ${PLIST} is not a valid plist:\n${_lint_output}")
endif()

# `raw` prints an array's length, so this is the count of document types.
execute_process(
    COMMAND plutil -extract CFBundleDocumentTypes raw -o - "${PLIST}"
    RESULT_VARIABLE _count_result
    OUTPUT_VARIABLE _count
    ERROR_VARIABLE  _count_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _count_result EQUAL 0 OR NOT _count MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "SpliceDocumentTypes: ${PLIST} declares no document types after splicing "
        "(${_count}${_count_error})")
endif()
