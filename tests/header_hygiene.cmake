# Header hygiene check for the public headers (include/nvrhi/**.h), run by ctest:
#
#     cmake -DNVRHI_INCLUDE_DIR=<nvrhi>/include -P header_hygiene.cmake
#
# The public headers must not use the standard library containers or other types whose layout differs between
# compilers and between debug and release runtimes. The allowed <memory> and <functional> pull in containers
# transitively on MSVC, so the check looks at direct includes and at tokens, after removing comments.
#
# It fails on:
# - an #include of <string> <vector> <optional> <array> <deque> <set> <map> <unordered_map> <list> <filesystem>
#   <mutex> (core/foundation.h, core/threading.h and core/memory.h may include <mutex>);
# - an #include of <memory> anywhere but core/autoptr.h;
# - a std::string / std::vector / std::optional / std::function / std::shared_ptr / std::unique_ptr /
#   std::weak_ptr token;
# - common/resource.h including core/foundation.h or core/autoptr.h;
# - the export rule (include/nvrhi/common/export.h): NVRHI_API is used only through NVRHI_C_API, and every
#   NVRHI_C_API / NVRHI_CORE_C_API declaration is noexcept.

if(NOT NVRHI_INCLUDE_DIR)
    message(FATAL_ERROR "header_hygiene.cmake: set NVRHI_INCLUDE_DIR")
endif()

file(GLOB_RECURSE headers RELATIVE "${NVRHI_INCLUDE_DIR}/nvrhi" "${NVRHI_INCLUDE_DIR}/nvrhi/*.h")
list(SORT headers)

set(forbidden_includes string vector optional array deque set map unordered_map list filesystem mutex)
set(mutex_allowed core/foundation.h core/threading.h core/memory.h)
set(forbidden_tokens string vector optional function shared_ptr unique_ptr weak_ptr)
set(export_headers common/export.h core/export.h)

set(violations "")

# Removes /* */ and // comments (string literals in these headers contain no comment markers).
function(strip_comments in_var out_var)
    set(text "${${in_var}}")
    while(TRUE)
        string(FIND "${text}" "/*" open)
        if(open EQUAL -1)
            break()
        endif()
        string(SUBSTRING "${text}" 0 ${open} head)
        math(EXPR rest_begin "${open} + 2")
        string(SUBSTRING "${text}" ${rest_begin} -1 rest)
        string(FIND "${rest}" "*/" close)
        if(close EQUAL -1)
            set(text "${head}")
            break()
        endif()
        math(EXPR tail_begin "${close} + 2")
        string(SUBSTRING "${rest}" ${tail_begin} -1 tail)
        set(text "${head} ${tail}")
    endwhile()
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

foreach(header IN LISTS headers)
    file(READ "${NVRHI_INCLUDE_DIR}/nvrhi/${header}" content)
    # Semicolons would split the text into a CMake list: '@' stands for them (no header uses '@' in code).
    string(REPLACE ";" "@" content "${content}")
    string(REPLACE "\r" "" content "${content}")
    strip_comments(content code)

    foreach(name IN LISTS forbidden_includes)
        if(name STREQUAL "mutex" AND header IN_LIST mutex_allowed)
            continue()
        endif()
        if(code MATCHES "#[ \t]*include[ \t]*<${name}>")
            list(APPEND violations "${header}: includes <${name}>")
        endif()
    endforeach()

    if(NOT header STREQUAL "core/autoptr.h" AND code MATCHES "#[ \t]*include[ \t]*<memory>")
        list(APPEND violations "${header}: includes <memory> (only core/autoptr.h may)")
    endif()

    foreach(token IN LISTS forbidden_tokens)
        if(code MATCHES "std[ \t]*::[ \t]*${token}([^A-Za-z0-9_]|$)")
            list(APPEND violations "${header}: uses std::${token}")
        endif()
    endforeach()

    if(header STREQUAL "common/resource.h" AND code MATCHES "#[ \t]*include[ \t]*<nvrhi/core/(foundation|autoptr)\\.h>")
        list(APPEND violations "${header}: includes core/foundation.h or core/autoptr.h")
    endif()

    if(NOT header IN_LIST export_headers)
        if(code MATCHES "(^|[^A-Za-z0-9_])NVRHI_API([^A-Za-z0-9_]|$)")
            list(APPEND violations "${header}: uses NVRHI_API (only NVRHI_C_API functions are exported)")
        endif()
        # Every C export declaration, up to its ';' ('@'), says noexcept.
        string(REGEX MATCHALL "NVRHI_(CORE_)?C_API[^@{}]*" exports "${code}")
        foreach(decl IN LISTS exports)
            if(NOT decl MATCHES "noexcept")
                string(REGEX REPLACE "[ \t\n]+" " " decl "${decl}")
                list(APPEND violations "${header}: C export without noexcept: ${decl}")
            endif()
        endforeach()
    endif()
endforeach()

list(LENGTH headers header_count)
if(violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR "Public header hygiene: violations found:\n  ${report}")
endif()
message(STATUS "Public header hygiene: ${header_count} headers checked, no violations")
