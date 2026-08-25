# Turns .po files into a C++ table. Runs in script mode (cmake -P).
#
# Inputs, all as -D arguments:
#   XPCOG_PO_NAME   the accessor's name, inside namespace xpcog::app
#   XPCOG_PO_FILES  semicolon-separated absolute paths, one .po per language
#   XPCOG_PO_CPP    where to write the definitions
#   XPCOG_PO_HPP    where to write the declaration
#
# Why this exists rather than msgfmt: gettext is not a tool this build needs
# anywhere else, and it would be the only one a developer has to install by hand
# -- on the platform where it is least likely to be there already. A build that
# cannot find it would either fail for a reason unrelated to what was being
# built, or fall back and produce a player that silently speaks English.
#
# What is emitted is *not* a .mo. It is the entries, verbatim: a .po already
# stores its strings C-escaped, so `"a\nb"` in the .po is `"a\nb"` in the
# generated source and the C++ compiler does the unescaping. This script
# therefore never decodes an escape, counts a byte, or writes a binary file from
# CMake -- three things it would have to get exactly right for a catalogue to
# load at all. Localization.cpp assembles the .mo image from the table at
# startup, where `strlen` is free and the offsets are covered by a test.
#
# It is a reader for the .po this project writes, not a general msgfmt. In
# particular a plural message must have every form filled in; a half-translated
# one is dropped rather than emitted with a hole in it.

foreach(_required NAME FILES CPP HPP)
    if(NOT DEFINED XPCOG_PO_${_required})
        message(FATAL_ERROR "CompileCatalog.cmake: XPCOG_PO_${_required} not set")
    endif()
endforeach()

# Two bytes that cannot occur in a .po, standing in for the two characters CMake
# would otherwise eat on the way through a list: `;` separates list elements and
# `\` escapes. Everything between the read below and the emit at the end works on
# the protected form, so a message containing either survives intact.
string(ASCII 1 _semicolon)
string(ASCII 2 _backslash)

# Closes the entry being accumulated, if there is one.
#
# A macro rather than a function because it is the accumulator: dynamic scoping
# is what lets it both read the fields the loop has been filling in and clear
# them. An untranslated message is dropped rather than emitted empty -- gettext
# leaves one as `msgstr ""`, and a catalogue answering with an empty string would
# replace a perfectly good English label with nothing at all.
macro(_xpcog_flush_entry)
    if(_have)
        set(_translated FALSE)
        foreach(_i RANGE 0 3)
            if(DEFINED _form_${_i} AND NOT _form_${_i} STREQUAL "")
                set(_translated TRUE)
            endif()
        endforeach()
        # The header -- msgid "" -- carries the charset and the plural rule
        # rather than a translation, and is kept whatever it holds.
        if(_id STREQUAL "" AND _context STREQUAL "")
            set(_translated TRUE)
        endif()
        # Every form or none: see the note at the top about what this is not.
        if(_translated AND _form_count GREATER 0)
            math(EXPR _wanted "${_form_count} - 1")
            foreach(_i RANGE 0 ${_wanted})
                if(NOT DEFINED _form_${_i} OR _form_${_i} STREQUAL "")
                    set(_translated FALSE)
                endif()
            endforeach()
        endif()

        if(_translated AND _form_count GREATER 0)
            set(_row "    {")
            if(_context STREQUAL "")
                string(APPEND _row "nullptr, ")
            else()
                string(APPEND _row "\"${_context}\", ")
            endif()
            string(APPEND _row "\"${_id}\", ")
            if(_plural STREQUAL "")
                string(APPEND _row "nullptr, {")
            else()
                string(APPEND _row "\"${_plural}\", {")
            endif()
            math(EXPR _wanted "${_form_count} - 1")
            foreach(_i RANGE 0 ${_wanted})
                string(APPEND _row "\"${_form_${_i}}\", ")
            endforeach()
            string(APPEND _row "nullptr}},\n")
            string(APPEND _rows "${_row}")
            math(EXPR _count "${_count} + 1")
        endif()
    endif()

    set(_have    FALSE)
    set(_context "")
    set(_id      "")
    set(_plural  "")
    set(_field   "")
    set(_form_count 0)
    foreach(_i RANGE 0 3)
        unset(_form_${_i})
    endforeach()
endmacro()

set(_catalogs "")
set(_tables "")

foreach(_po IN LISTS XPCOG_PO_FILES)
    if(NOT EXISTS "${_po}")
        message(FATAL_ERROR "CompileCatalog.cmake: no such file: ${_po}")
    endif()

    # The language is the file's own name: es.po is Spanish, pt_BR.po would be
    # Brazilian Portuguese. Nothing inside the file names it in a form anything
    # reads -- the `Language:` header is documentation -- so the filename is the
    # one spelling that cannot drift from what the build compiled in.
    get_filename_component(_language "${_po}" NAME_WE)

    file(READ "${_po}" _text)
    string(REPLACE "\\" "${_backslash}" _text "${_text}")
    string(REPLACE ";" "${_semicolon}" _text "${_text}")
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(REPLACE "\n" ";" _text "${_text}")

    # `_have` distinguishes "no entry yet" from an entry whose msgid is the empty
    # string, which is the header and must not be dropped.
    set(_have FALSE)
    set(_count 0)
    set(_rows "")
    _xpcog_flush_entry()

    foreach(_line IN LISTS _text)
        string(STRIP "${_line}" _line)

        if(_line STREQUAL "")
            _xpcog_flush_entry()
            continue()
        endif()
        if(_line MATCHES "^#")
            # Comments, including the `#~` ones gettext writes for a message that
            # has left the source. Those carry a whole msgid/msgstr pair, so they
            # have to be skipped as comments rather than parsed as messages.
            continue()
        endif()

        # The text between the first and the last quote on the line. Greedy on
        # purpose: an escaped quote inside the string is a real `"` by now, so
        # the closing quote is the last one rather than the second.
        string(REGEX REPLACE "^[^\"]*\"(.*)\"[^\"]*$" "\\1" _content "${_line}")

        if(_line MATCHES "^msgctxt[ \t]")
            _xpcog_flush_entry()
            set(_have TRUE)
            set(_context "${_content}")
            set(_field context)
        elseif(_line MATCHES "^msgid_plural[ \t]")
            set(_plural "${_content}")
            set(_field plural)
        elseif(_line MATCHES "^msgid[ \t]")
            # A msgid directly after a msgctxt belongs to it; one after anything
            # else starts a fresh entry.
            if(NOT _field STREQUAL "context")
                _xpcog_flush_entry()
            endif()
            set(_have TRUE)
            set(_id "${_content}")
            set(_field id)
        elseif(_line MATCHES "^msgstr(\\[[0-9]+\\])?[ \t]")
            if(_form_count LESS 4)
                set(_form_${_form_count} "${_content}")
                math(EXPR _form_count "${_form_count} + 1")
            else()
                message(FATAL_ERROR
                    "CompileCatalog.cmake: ${_po}: more than four plural forms")
            endif()
            set(_field form)
        elseif(_line MATCHES "^\"")
            # A continuation: gettext splits a long string across lines and the
            # pieces are concatenated with nothing between them.
            if(_field STREQUAL "context")
                string(APPEND _context "${_content}")
            elseif(_field STREQUAL "id")
                string(APPEND _id "${_content}")
            elseif(_field STREQUAL "plural")
                string(APPEND _plural "${_content}")
            elseif(_field STREQUAL "form")
                math(EXPR _last "${_form_count} - 1")
                string(APPEND _form_${_last} "${_content}")
            endif()
        else()
            message(FATAL_ERROR
                "CompileCatalog.cmake: ${_po}: cannot read line: ${_line}")
        endif()
    endforeach()

    _xpcog_flush_entry()

    if(_count EQUAL 0)
        message(FATAL_ERROR "CompileCatalog.cmake: ${_po} holds no messages")
    endif()

    # Back to real semicolons and backslashes, once, at the point the accumulated
    # text stops being a CMake list and becomes C++.
    string(REPLACE "${_semicolon}" ";" _rows "${_rows}")
    string(REPLACE "${_backslash}" "\\" _rows "${_rows}")

    string(MAKE_C_IDENTIFIER "${_language}" _symbol)
    string(APPEND _tables
        "const CatalogEntry k_${_symbol}[] = {\n${_rows}};\n\n")
    string(APPEND _catalogs "    {\"${_language}\", k_${_symbol}},\n")

    message(STATUS "XPCog: ${_language} catalogue, ${_count} messages")
endforeach()

file(WRITE "${XPCOG_PO_HPP}"
"// Generated by cmake/CompileCatalog.cmake from app/locale/*.po. Do not edit.
#pragma once

#include <span>

namespace xpcog::app {

/// One message, exactly as the .po spelled it.
///
/// The strings are the .po's own escaped text handed to the C++ compiler
/// unchanged, so their lengths are whatever strlen says at run time. Nothing in
/// the build counts a byte, which is what lets the generator be a CMake script
/// rather than a program.
struct CatalogEntry {
    const char* context;   ///< nullptr unless the message has a msgctxt
    const char* singular;  ///< the msgid; empty for the catalogue header
    const char* plural;    ///< nullptr unless the message has a msgid_plural
    const char* forms[4];  ///< msgstr, or msgstr[0..n]; nullptr-terminated
};

struct Catalog {
    const char*                   language;  ///< the .po's basename, e.g. \"es\"
    std::span<const CatalogEntry> entries;
};

/// Every catalogue compiled into this build, in the order the build listed them.
[[nodiscard]] std::span<const Catalog> ${XPCOG_PO_NAME}();

}  // namespace xpcog::app
")

file(WRITE "${XPCOG_PO_CPP}"
"// Generated by cmake/CompileCatalog.cmake from app/locale/*.po. Do not edit.
#include \"${XPCOG_PO_NAME}.hpp\"

namespace xpcog::app {
namespace {

${_tables}const Catalog kCatalogs[] = {
${_catalogs}};

}  // namespace

std::span<const Catalog> ${XPCOG_PO_NAME}() { return kCatalogs; }

}  // namespace xpcog::app
")
