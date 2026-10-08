# EPM.cmake - Ethereal Package Manager. Configure-time dependency and asset management in
# the manner of CPM.cmake, without selecting or invoking an external package manager.
#
# The one copy lives in ethereal-nvrhi/cmake/EPM.cmake; every project includes it by path:
#   include("${CMAKE_CURRENT_LIST_DIR}/.../ethereal-nvrhi/cmake/EPM.cmake")
# Including it again is a no-op.
#
# ======================================================================================
# epm_add_package - code that is built as part of this project
# ======================================================================================
#   epm_add_package("gh:nlohmann/json@3.11.3")                       # shorthand
#   epm_add_package(
#       NAME Vulkan-Headers PACKAGE VulkanHeaders VERSION 1.4.352...1.4.65535
#       FIND_PACKAGE_ARGUMENTS CONFIG
#       GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG v1.4.352
#       OPTIONS "VULKAN_HEADERS_ENABLE_TESTS OFF")
#   epm_add_package(NAME glfw SOURCE_DIR thirdparty/glfw TARGETS glfw OPTIONS GLFW_BUILD_TESTS=OFF)
#   epm_add_package(NAME stb SOURCE_DIR thirdparty/stb DOWNLOAD_ONLY)
#
# Resolution order:
#   1. EPM_<NAME>_SOURCE=<dir>   a local checkout overrides everything (development)
#   2. TARGETS                   all listed targets already exist
#   3. an installed package      find_package, when PACKAGE or FIND_PACKAGE_ARGUMENTS is given,
#                                the call is epm_find_package, or EPM_USE_LOCAL_PACKAGES=ON
#   4. SOURCE_DIR                a source tree in this repository, normally a git submodule
#   5. GIT_REPOSITORY / URL      fetched into the shared source cache
#
# Arguments:
#   NAME                    Package name; prefixes the result variables.
#   VERSION                 find_package minimum or range. A plain "1.2.3" also selects the
#                           git tag "v1.2.3" when GIT_TAG is absent.
#   PACKAGE, FIND_PACKAGE_ARGUMENTS   how to look for an installed package.
#   TARGETS                 Targets that mean "already provided".
#   GIT_REPOSITORY | GITHUB_REPOSITORY | GITLAB_REPOSITORY | BITBUCKET_REPOSITORY, GIT_TAG
#                           Fetch from git (tag, branch or commit). GIT_SHALLOW defaults to
#                           TRUE; GIT_SUBMODULES takes paths, or ALL; SPARSE_PATHS limits
#                           the checkout.
#   URL, URL_HASH           Fetch an archive (hash is ALGO=hex and is required unless
#                           ALLOW_UNVERIFIED); FILENAME names it when the URL does not.
#   SOURCE_DIR              A source tree already in this repository (relative paths are
#                           relative to the calling directory); missing or empty is an error
#                           that points at `git submodule update`. It is built in the
#                           matching directory of the build tree, as add_subdirectory would.
#   BINARY_DIR              Build directory for the package, when the default is not wanted.
#   FETCH_DIR               Fetch into this fixed directory instead of the shared cache; inside
#                           the calling directory it is built in the matching build directory.
#   SOURCE_SUBDIR           Directory holding the CMakeLists.txt inside the tree.
#   DOWNLOAD_ONLY           Make the sources available, add_subdirectory nothing.
#   EXCLUDE_FROM_ALL, SYSTEM   Passed to add_subdirectory.
#   OPTIONS                 "KEY VALUE" or KEY=VALUE cache variables set before the source is
#                           added (not applied to an installed package).
#   PATCHES                 Patch files applied with `git apply` to a fresh tree.
#   NO_CACHE                Keep this package in the build tree, not in EPM_SOURCE_CACHE.
#   OPTIONAL                Not finding it is not an error: <NAME>_FOUND is FALSE.
#
# Results: <NAME>_FOUND, <NAME>_ADDED, <NAME>_SOURCE (installed|target|local|submodule|
# existing|cached|downloaded|none), <NAME>_SOURCE_DIR, <NAME>_BINARY_DIR, <NAME>_VERSION and
# EPM_LAST_PACKAGE_NAME. A package is added once; the first request wins, and a later one
# that wants a newer VERSION or another source warns (an error with EPM_STRICT=ON).
#
# ======================================================================================
# The rest of the API
# ======================================================================================
#   epm_declare_package(NAME x ...)       remember arguments for epm_add_package(x)
#   epm_find_package(NAME x ...)          same arguments as epm_add_package, but always look
#                                         for an installed package first
#   epm_add_asset(NAME x ...)             files that are only downloaded or checked out: SDKs,
#                                         media, archives, git trees. Takes the arguments of
#                                         epm_add_package that fetch (URL, GIT_*, SPARSE_PATHS,
#                                         PATCHES, ...) plus
#                                           DESTINATION  fetch into this directory (= FETCH_DIR)
#                                           NO_EXTRACT   keep an archive as a file
#                                         and sets <NAME>_DIR and <NAME>_FILE as well.
#   epm_use_package_lock(<file>)          pin fetched sources to the revisions in a lock file
#   epm_write_package_lock(<file>)        write that file for every package fetched so far
#   epm_print_summary()                   table of what was resolved and from where
#
# Content EPM did not create in a FETCH_DIR (an existing clone, a manual download) is used
# as it is and never modified. Archives are extracted once; a single top-level directory is
# stripped, as FetchContent does.
#
# Switches (cache variables or environment variables):
#   EPM_SOURCE_CACHE         shared source and download cache; default <build>/_epm/src
#   EPM_USE_LOCAL_PACKAGES   try find_package for every package (default OFF)
#   EPM_LOCAL_PACKAGES_ONLY  a package find_package cannot find is an error
#   EPM_DOWNLOAD_ALL, EPM_DOWNLOAD_<NAME>   never use installed packages (NAME upper-cased)
#   EPM_<NAME>_SOURCE        use this local directory for one package or asset
#   EPM_OFFLINE              never touch the network; anything missing is an error
#   EPM_ALLOW_UNVERIFIED     accept URL downloads without a hash
#   EPM_STRICT               version or source conflicts between requests are errors
#   EPM_URL_REWRITE          "prefix=replacement" pairs for git and download URLs (mirrors)
#   EPM_DOWNLOAD_RETRIES     extra download attempts (default 2)
#   EPM_DRY_RUN              print what would be done, fetch and add nothing
#   EPM_SHOW_SUMMARY         print the summary at the end of the configure
#   EPM_UPDATE_LOCK_FILE     write a lock file at the end of the configure

if (CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "EPM.cmake needs CMake 3.21 or newer (running ${CMAKE_VERSION})")
endif()
get_property(_epm_loaded GLOBAL PROPERTY EPM_LOADED)
if (_epm_loaded)
    return()
endif()
set_property(GLOBAL PROPERTY EPM_LOADED TRUE)

# ======================================================================================
# helpers
# ======================================================================================

function(_epm_upper out name)
    string(TOUPPER "${name}" name)
    string(MAKE_C_IDENTIFIER "${name}" name)
    set(${out} "${name}" PARENT_SCOPE)
endfunction()

# A setting is a cache or normal variable, else an environment variable.
function(_epm_setting out var)
    if (DEFINED ${var})
        set(${out} "${${var}}" PARENT_SCOPE)
    else()
        set(${out} "$ENV{${var}}" PARENT_SCOPE)
    endif()
endfunction()

# Per-package registry, in global properties.
function(_epm_set name key value)
    set_property(GLOBAL PROPERTY EPM_PKG_${name}_${key} "${value}")
endfunction()
function(_epm_get out name key)
    get_property(_v GLOBAL PROPERTY EPM_PKG_${name}_${key})
    set(${out} "${_v}" PARENT_SCOPE)
endfunction()

function(_epm_is_version out text)
    if (text MATCHES "^[0-9]+(\\.[0-9]+)*$")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_epm_cache_root out no_cache)
    _epm_setting(_dir EPM_SOURCE_CACHE)
    if (no_cache OR NOT _dir)
        set(_dir "${CMAKE_BINARY_DIR}/_epm/src")
    endif()
    file(TO_CMAKE_PATH "${_dir}" _dir)
    set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# Apply the first matching "prefix=replacement" rule of EPM_URL_REWRITE.
function(_epm_rewrite_url out url)
    _epm_setting(_rules EPM_URL_REWRITE)
    foreach (_rule IN LISTS _rules)
        string(FIND "${_rule}" "=" _eq)
        if (_eq LESS 1)
            message(FATAL_ERROR "EPM_URL_REWRITE entries must be prefix=replacement (got '${_rule}')")
        endif()
        string(SUBSTRING "${_rule}" 0 ${_eq} _from)
        math(EXPR _eq "${_eq} + 1")
        string(SUBSTRING "${_rule}" ${_eq} -1 _to)
        string(LENGTH "${_from}" _len)
        string(SUBSTRING "${url}" 0 ${_len} _head)
        if (_head STREQUAL _from)
            string(SUBSTRING "${url}" ${_len} -1 _tail)
            set(url "${_to}${_tail}")
            break()
        endif()
    endforeach()
    set(${out} "${url}" PARENT_SCOPE)
endfunction()

# "KEY VALUE" or "KEY=VALUE" -> cache entry; ON/OFF/TRUE/FALSE become BOOL so option() and
# the package's own set(... CACHE BOOL ...) agree on the type.
function(_epm_apply_options name)
    foreach (_opt IN LISTS ARGN)
        if (NOT _opt MATCHES "^([^ =]+)(=| +)(.*)$")
            message(FATAL_ERROR "EPM: ${name}: OPTIONS entries are 'KEY VALUE' or KEY=VALUE (got '${_opt}')")
        endif()
        set(_key "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_3}")
        set(_type STRING)
        if (_value MATCHES "^(ON|OFF|TRUE|FALSE)$")
            set(_type BOOL)
        endif()
        set(${_key} "${_value}" CACHE ${_type} "" FORCE)
    endforeach()
endfunction()

# A conflicting second request is a warning, or an error with EPM_STRICT.
function(_epm_conflict text)
    _epm_setting(_strict EPM_STRICT)
    if (_strict)
        message(FATAL_ERROR "EPM: ${text}")
    endif()
    message(WARNING "EPM: ${text}")
endfunction()

# Content directories carry a `.resolved` stamp holding the identity of what is inside,
# written last: current | stale | foreign | missing.
#   stale    EPM created it and it is out of date or half-written: safe to replace
#   foreign  somebody else's content: never touched
function(_epm_stamp_state out dir identity)
    set(_state missing)
    if (EXISTS "${dir}/.resolving")
        set(_state stale)
    elseif (EXISTS "${dir}/.resolved")
        file(READ "${dir}/.resolved" _have)
        if (_have STREQUAL identity)
            set(_state current)
        else()
            set(_state stale)
        endif()
    elseif (IS_DIRECTORY "${dir}")
        file(GLOB _any "${dir}/*")
        if (_any)
            set(_state foreign)
        endif()
    endif()
    set(${out} ${_state} PARENT_SCOPE)
endfunction()

# --- fetching -----------------------------------------------------------------------------

function(_epm_git dir)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _res ERROR_VARIABLE _err OUTPUT_QUIET)
    if (NOT _res EQUAL 0)
        message(FATAL_ERROR "EPM: git ${ARGN} failed in ${dir}: ${_err}")
    endif()
endfunction()

# Check out one ref (tag, branch or commit) into a fresh dir.
#   _epm_git_checkout(dir url ref SHALLOW <bool> [SPARSE paths...] [SUBMODULES paths...])
function(_epm_git_checkout dir url ref)
    cmake_parse_arguments(G "" "SHALLOW" "SPARSE;SUBMODULES" ${ARGN})
    find_package(Git QUIET)
    if (NOT GIT_FOUND)
        message(FATAL_ERROR "EPM: git is required to fetch ${url}")
    endif()
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/.resolving" "")
    _epm_git("${dir}" init -q)
    _epm_git("${dir}" remote add origin "${url}")
    set(_filter "")
    if (G_SPARSE)
        _epm_git("${dir}" sparse-checkout set --no-cone ${G_SPARSE})
        set(_filter --filter=blob:none)
    endif()
    set(_depth "")
    if (G_SHALLOW)
        set(_depth --depth 1)
    endif()
    _epm_git("${dir}" fetch -q ${_depth} ${_filter} origin "${ref}")
    _epm_git("${dir}" -c advice.detachedHead=false checkout -q FETCH_HEAD)
    if (G_SUBMODULES)
        list(REMOVE_ITEM G_SUBMODULES ALL)
        _epm_git("${dir}" submodule update --init --recursive ${_depth} ${G_SUBMODULES})
    endif()
endfunction()

# Download url to file unless it already matches algo=hash. downloaded = TRUE/FALSE.
function(_epm_download downloaded name url file algo hash)
    if (EXISTS "${file}")
        set(_ok TRUE)
        if (algo)
            file(${algo} "${file}" _actual)
            string(TOLOWER "${_actual}" _actual)
            if (NOT _actual STREQUAL hash)
                message(STATUS "EPM: ${name}: cached file fails its ${algo} check, downloading again")
                set(_ok FALSE)
            endif()
        endif()
        if (_ok)
            set(${downloaded} FALSE PARENT_SCOPE)
            return()
        endif()
    endif()
    _epm_setting(_offline EPM_OFFLINE)
    if (_offline)
        message(FATAL_ERROR "EPM: ${name} is not cached at '${file}' and EPM_OFFLINE is ON")
    endif()
    _epm_setting(_retries EPM_DOWNLOAD_RETRIES)
    if ("${_retries}" STREQUAL "")
        set(_retries 2)
    endif()
    set(_hash_args "")
    if (algo)
        set(_hash_args EXPECTED_HASH ${algo}=${hash})
    endif()
    set(_attempt 0)
    while (TRUE)
        message(STATUS "EPM: downloading ${name} from ${url}")
        # A temporary name, so an interrupted transfer never looks cached.
        file(DOWNLOAD "${url}" "${file}.part" ${_hash_args}
             STATUS _status SHOW_PROGRESS TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
        list(GET _status 0 _code)
        list(GET _status 1 _text)
        if (_code EQUAL 0)
            break()
        endif()
        file(REMOVE "${file}.part")
        math(EXPR _attempt "${_attempt} + 1")
        if (_text MATCHES "HASH mismatch" OR _attempt GREATER _retries)
            message(FATAL_ERROR "EPM: ${name}: download failed (${_code}: ${_text}) ${url}")
        endif()
        message(STATUS "EPM: ${name}: download failed (${_text}), retrying")
    endwhile()
    file(RENAME "${file}.part" "${file}")
    set(${downloaded} TRUE PARENT_SCOPE)
endfunction()

# Extract archive into dest, stripping a single top-level directory.
function(_epm_extract archive dest)
    set(_tmp "${dest}.extracting")
    file(REMOVE_RECURSE "${_tmp}" "${dest}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${_tmp}")
    file(GLOB _children LIST_DIRECTORIES TRUE "${_tmp}/*")
    list(LENGTH _children _count)
    get_filename_component(_parent "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent}")
    if (_count EQUAL 1 AND IS_DIRECTORY "${_children}")
        file(RENAME "${_children}" "${dest}")
        file(REMOVE_RECURSE "${_tmp}")
    else()
        file(RENAME "${_tmp}" "${dest}")
    endif()
endfunction()

function(_epm_apply_patches dir)
    find_package(Git QUIET)
    foreach (_patch IN LISTS ARGN)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_patch}"
            WORKING_DIRECTORY "${dir}" RESULT_VARIABLE _res ERROR_VARIABLE _err OUTPUT_QUIET)
        if (NOT _res EQUAL 0)
            message(FATAL_ERROR "EPM: patch ${_patch} does not apply to ${dir}: ${_err}")
        endif()
    endforeach()
endfunction()

# Make a git ref or an archive available on disk; identity is the cache key.
#   _epm_populate(<name> <identity> OUT_DIR v OUT_FILE v OUT_STATE v
#                 (GIT_REPOSITORY u GIT_TAG t ... | URL u ALGO a HASH h ...) [FETCH_DIR d])
# State: existing | cached | downloaded.
function(_epm_populate name identity)
    cmake_parse_arguments(PP "NO_CACHE;NO_EXTRACT"
        "OUT_DIR;OUT_FILE;OUT_STATE;GIT_REPOSITORY;GIT_TAG;GIT_SHALLOW;URL;ALGO;HASH;FILENAME;FETCH_DIR"
        "SPARSE_PATHS;GIT_SUBMODULES;PATCHES" ${ARGN})

    _epm_cache_root(_root "${PP_NO_CACHE}")
    string(SHA256 _key "${identity}")
    string(SUBSTRING "${_key}" 0 12 _key)
    set(_base "${_root}/${name}/${_key}")
    set(_dir "${PP_FETCH_DIR}")
    if (NOT _dir)
        set(_dir "${_base}/src")
    endif()
    set(_file "")
    set(_state cached)

    set(_extract FALSE)
    if (PP_URL)
        if (NOT PP_FILENAME)
            string(REGEX REPLACE "[?#].*$" "" _path "${PP_URL}")
            get_filename_component(PP_FILENAME "${_path}" NAME)
        endif()
        if (NOT PP_FILENAME)
            message(FATAL_ERROR "EPM: ${name}: cannot derive a file name from the URL, pass FILENAME")
        endif()
        set(_file "${_base}/${PP_FILENAME}")
        if (NOT PP_NO_EXTRACT AND PP_FILENAME MATCHES "\\.(zip|7z|tar|tar\\.gz|tgz|tar\\.bz2|tar\\.xz|txz)$")
            set(_extract TRUE)
        endif()
    endif()

    if (PP_GIT_REPOSITORY OR _extract)
        # a git tree or an extracted archive: a stamped directory
        _epm_stamp_state(_s "${_dir}" "${identity}")
        if (_s STREQUAL foreign)
            message(STATUS "EPM: ${name}: using existing ${_dir}")
            set(_state existing)
        elseif (NOT _s STREQUAL current)
            if (PP_GIT_REPOSITORY)
                _epm_setting(_offline EPM_OFFLINE)
                if (_offline)
                    message(FATAL_ERROR "EPM: ${name} is not cached at '${_dir}' and EPM_OFFLINE is ON")
                endif()
                message(STATUS "EPM: fetching ${name} from ${PP_GIT_REPOSITORY} (${PP_GIT_TAG})")
                set(_shallow TRUE)
                if (NOT "${PP_GIT_SHALLOW}" STREQUAL "")
                    set(_shallow ${PP_GIT_SHALLOW})
                endif()
                _epm_git_checkout("${_dir}" "${PP_GIT_REPOSITORY}" "${PP_GIT_TAG}" SHALLOW "${_shallow}"
                    SPARSE ${PP_SPARSE_PATHS} SUBMODULES ${PP_GIT_SUBMODULES})
                file(REMOVE "${_dir}/.resolving")
                set(_state downloaded)
            else()
                _epm_download(_got ${name} "${PP_URL}" "${_file}" "${PP_ALGO}" "${PP_HASH}")
                _epm_extract("${_file}" "${_dir}")
                if (_got)
                    set(_state downloaded)
                endif()
            endif()
            if (PP_PATCHES)
                _epm_apply_patches("${_dir}" ${PP_PATCHES})
            endif()
            file(WRITE "${_dir}/.resolved" "${identity}")
        endif()
    else()
        # a plain file, copied to FETCH_DIR when one is given
        _epm_download(_got ${name} "${PP_URL}" "${_file}" "${PP_ALGO}" "${PP_HASH}")
        if (_got)
            set(_state downloaded)
        endif()
        set(_dir "${_base}")
        if (PP_FETCH_DIR)
            file(MAKE_DIRECTORY "${PP_FETCH_DIR}")
            file(COPY_FILE "${_file}" "${PP_FETCH_DIR}/${PP_FILENAME}" ONLY_IF_DIFFERENT)
            set(_dir "${PP_FETCH_DIR}")
            set(_file "${PP_FETCH_DIR}/${PP_FILENAME}")
        endif()
    endif()

    set(${PP_OUT_DIR} "${_dir}" PARENT_SCOPE)
    set(${PP_OUT_FILE} "${_file}" PARENT_SCOPE)
    set(${PP_OUT_STATE} "${_state}" PARENT_SCOPE)
endfunction()

# --- argument handling --------------------------------------------------------------------

# gh:user/repo[@version-or-tag][#tag] (also gl:, bb:) -> keyword arguments.
function(_epm_expand_uri out uri)
    if (NOT uri MATCHES "^(gh|gl|bb):([^@#]+)(@([^#]+))?(#(.+))?$")
        message(FATAL_ERROR "EPM: unsupported URI '${uri}' (expected gh:user/repo[@version|@tag][#tag])")
    endif()
    set(_scheme "${CMAKE_MATCH_1}")
    set(_repo "${CMAKE_MATCH_2}")
    set(_at "${CMAKE_MATCH_4}")
    set(_tag "${CMAKE_MATCH_6}")
    set(_keys gh GITHUB_REPOSITORY gl GITLAB_REPOSITORY bb BITBUCKET_REPOSITORY)
    list(FIND _keys ${_scheme} _i)
    math(EXPR _i "${_i} + 1")
    list(GET _keys ${_i} _key)
    get_filename_component(_name "${_repo}" NAME)
    set(_args NAME "${_name}" ${_key} "${_repo}")
    if (_at MATCHES "^[0-9]+(\\.[0-9]+)*$")
        list(APPEND _args VERSION "${_at}")
    elseif (_at)
        list(APPEND _args GIT_TAG "${_at}")
    endif()
    if (_tag)
        list(APPEND _args GIT_TAG "${_tag}")
    endif()
    set(${out} "${_args}" PARENT_SCOPE)
endfunction()

# A lone URI or declared name, the URI keyword and declared defaults -> one argument list.
function(_epm_normalize out)
    set(_args ${ARGN})
    list(LENGTH _args _n)
    if (_n EQUAL 1)
        if (_args MATCHES "^(gh|gl|bb):")
            _epm_expand_uri(_args "${_args}")
        else()
            set(_args NAME "${_args}")
        endif()
    endif()
    cmake_parse_arguments(PRE "" "NAME;URI" "" ${_args})
    if (PRE_URI)
        _epm_expand_uri(_more "${PRE_URI}")
        set(_args ${_more} ${_args})
        cmake_parse_arguments(PRE "" "NAME" "" ${_args})
    endif()
    get_property(_declared GLOBAL PROPERTY EPM_DECLARED_${PRE_NAME})
    set(${out} ${_declared} ${_args} PARENT_SCOPE)    # explicit arguments come last and win
endfunction()

# add_subdirectory when the tree has a CMakeLists.txt; otherwise it is just sources.
function(_epm_add_subdirectory name dir subdir bin exclude system)
    if (subdir)
        set(dir "${dir}/${subdir}")
    endif()
    if (NOT EXISTS "${dir}/CMakeLists.txt")
        message(STATUS "EPM: ${name}: no CMakeLists.txt in ${dir}, sources only")
        return()
    endif()
    set(_args "${dir}" "${bin}")
    if (exclude)
        list(APPEND _args EXCLUDE_FROM_ALL)
    endif()
    if (system AND NOT CMAKE_VERSION VERSION_LESS 3.25)
        list(APPEND _args SYSTEM)
    endif()
    add_subdirectory(${_args})
endfunction()

# ======================================================================================
# the engine behind epm_add_package, epm_find_package and epm_add_asset
# ======================================================================================

function(_epm_add kind try_find)
    _epm_normalize(_args ${ARGN})
    cmake_parse_arguments(P
        "DOWNLOAD_ONLY;EXCLUDE_FROM_ALL;SYSTEM;OPTIONAL;ALLOW_UNVERIFIED;NO_CACHE;NO_EXTRACT"
        "NAME;URI;VERSION;PACKAGE;SOURCE_DIR;BINARY_DIR;SOURCE_SUBDIR;FETCH_DIR;DESTINATION;FILENAME;URL;URL_HASH;GIT_REPOSITORY;GIT_TAG;GIT_SHALLOW;GITHUB_REPOSITORY;GITLAB_REPOSITORY;BITBUCKET_REPOSITORY"
        "OPTIONS;FIND_PACKAGE_ARGUMENTS;TARGETS;PATCHES;SPARSE_PATHS;GIT_SUBMODULES" ${_args})

    if (P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "EPM: unknown arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    if (NOT P_NAME)
        message(FATAL_ERROR "EPM: NAME is required (or a gh:user/repo URI)")
    endif()
    set(name "${P_NAME}")
    _epm_upper(_u "${name}")
    set_property(GLOBAL PROPERTY EPM_LAST "${name}")

    # A lock file pins where the sources come from.
    get_property(_lock GLOBAL PROPERTY EPM_LOCK_${name})
    if (_lock)
        cmake_parse_arguments(L "" "GIT_REPOSITORY;GIT_TAG;URL;URL_HASH" "" ${_lock})
        if (L_GIT_REPOSITORY)
            set(P_GIT_REPOSITORY "${L_GIT_REPOSITORY}")
            set(P_GIT_TAG "${L_GIT_TAG}")
            set(P_GITHUB_REPOSITORY "")
            set(P_GITLAB_REPOSITORY "")
            set(P_BITBUCKET_REPOSITORY "")
        elseif (L_URL)
            set(P_URL "${L_URL}")
            set(P_URL_HASH "${L_URL_HASH}")
        endif()
    endif()

    if (NOT P_FETCH_DIR)
        set(P_FETCH_DIR "${P_DESTINATION}")
    endif()
    if (P_SOURCE_DIR)
        get_filename_component(P_SOURCE_DIR "${P_SOURCE_DIR}" ABSOLUTE)
    endif()
    if (NOT P_GIT_REPOSITORY)
        if (P_GITHUB_REPOSITORY)
            set(P_GIT_REPOSITORY "https://github.com/${P_GITHUB_REPOSITORY}.git")
        elseif (P_GITLAB_REPOSITORY)
            set(P_GIT_REPOSITORY "https://gitlab.com/${P_GITLAB_REPOSITORY}.git")
        elseif (P_BITBUCKET_REPOSITORY)
            set(P_GIT_REPOSITORY "https://bitbucket.org/${P_BITBUCKET_REPOSITORY}.git")
        endif()
    endif()
    if (P_GIT_REPOSITORY AND P_URL)
        message(FATAL_ERROR "EPM: ${name}: give a git repository or a URL, not both")
    endif()
    if (P_GIT_REPOSITORY AND NOT P_GIT_TAG)
        _epm_is_version(_plain "${P_VERSION}")
        if (NOT _plain)
            message(FATAL_ERROR "EPM: ${name}: GIT_TAG is required (VERSION '${P_VERSION}' is not a plain version)")
        endif()
        set(P_GIT_TAG "v${P_VERSION}")
    endif()

    # The identity says what the sources are; the lock file records the original URLs.
    set(_orig_repo "${P_GIT_REPOSITORY}")
    set(_orig_url "${P_URL}")
    set(_identity "")
    if (P_GIT_REPOSITORY)
        set(_identity "git|${_orig_repo}|${P_GIT_TAG}")
        _epm_rewrite_url(P_GIT_REPOSITORY "${P_GIT_REPOSITORY}")
    elseif (P_URL)
        set(_identity "url|${_orig_url}|${P_URL_HASH}")
        _epm_rewrite_url(P_URL "${P_URL}")
    elseif (P_SOURCE_DIR)
        set(_identity "dir|${P_SOURCE_DIR}")
    endif()

    # Added before: the first request wins.
    _epm_get(_registered ${name} REGISTERED)
    if (_registered)
        _epm_get(_prev_version ${name} VERSION)
        _epm_get(_prev_identity ${name} IDENTITY)
        _epm_is_version(_new "${P_VERSION}")
        _epm_is_version(_old "${_prev_version}")
        if (_new AND _old AND _prev_version VERSION_LESS P_VERSION)
            _epm_conflict("${name} ${P_VERSION} was requested, but ${_prev_version} was already added")
        elseif (_identity AND _prev_identity AND NOT _identity STREQUAL _prev_identity)
            _epm_conflict("${name} was already added from '${_prev_identity}'; ignoring '${_identity}'")
        endif()
        return()
    endif()

    set(_source "")
    set(_src_dir "")
    set(_file "")
    set(_version "${P_VERSION}")
    set(_bin_dir "${CMAKE_BINARY_DIR}/_epm/${name}-build")
    if (kind STREQUAL asset)
        set(P_DOWNLOAD_ONLY TRUE)
    endif()

    # 1. a local checkout overrides everything
    if (EPM_${_u}_SOURCE)
        if (NOT IS_DIRECTORY "${EPM_${_u}_SOURCE}")
            message(FATAL_ERROR "EPM_${_u}_SOURCE='${EPM_${_u}_SOURCE}' is not a directory")
        endif()
        message(STATUS "EPM: ${name}: local source ${EPM_${_u}_SOURCE}")
        set(_source local)
        set(_src_dir "${EPM_${_u}_SOURCE}")
    endif()

    # An asset is never an installed package; EPM_DOWNLOAD_* rule installed packages out.
    _epm_setting(_download_all EPM_DOWNLOAD_ALL)
    _epm_setting(_download_this EPM_DOWNLOAD_${_u})
    set(_may_use_installed TRUE)
    if (_download_all OR _download_this OR kind STREQUAL asset)
        set(_may_use_installed FALSE)
    endif()

    # 2. already provided
    if (NOT _source AND P_TARGETS AND _may_use_installed)
        set(_all TRUE)
        foreach (_t IN LISTS P_TARGETS)
            if (NOT TARGET ${_t})
                set(_all FALSE)
            endif()
        endforeach()
        if (_all)
            message(STATUS "EPM: ${name}: using existing target(s) ${P_TARGETS}")
            set(_source target)
        endif()
    endif()

    # 3. an installed package (vcpkg, a registry, ...)
    _epm_setting(_use_local EPM_USE_LOCAL_PACKAGES)
    if (NOT _source AND _may_use_installed
            AND (try_find OR P_PACKAGE OR P_FIND_PACKAGE_ARGUMENTS OR _use_local))
        set(_fname "${P_PACKAGE}")
        if (NOT _fname)
            set(_fname "${name}")
        endif()
        find_package(${_fname} ${P_VERSION} ${P_FIND_PACKAGE_ARGUMENTS} QUIET)
        _epm_setting(_local_only EPM_LOCAL_PACKAGES_ONLY)
        if (${_fname}_FOUND)
            message(STATUS "EPM: ${name}: using installed ${_fname} ${${_fname}_VERSION}")
            set(_source installed)
            if (${_fname}_VERSION)
                set(_version "${${_fname}_VERSION}")
            endif()
        elseif (_local_only)
            message(FATAL_ERROR "EPM: ${name}: EPM_LOCAL_PACKAGES_ONLY is ON but find_package(${_fname} ${P_VERSION}) found nothing")
        endif()
    endif()

    _epm_setting(_dry_run EPM_DRY_RUN)
    if (_dry_run AND NOT _source)
        message(STATUS "EPM (dry run): would add ${name} from ${_identity}")
        return()
    endif()

    # 4. a source tree in this repository
    if (NOT _source AND P_SOURCE_DIR)
        file(GLOB _contents "${P_SOURCE_DIR}/*")    # an uninitialised submodule is an empty directory
        if (NOT _contents)
            message(FATAL_ERROR "EPM: ${name}: '${P_SOURCE_DIR}' is missing or empty. If it is a git "
                "submodule, run: git submodule update --init --recursive")
        endif()
        message(STATUS "EPM: ${name}: source ${P_SOURCE_DIR}")
        set(_source submodule)
        set(_src_dir "${P_SOURCE_DIR}")
    endif()

    # 5. fetch
    if (NOT _source AND (P_GIT_REPOSITORY OR P_URL))
        set(_algo "")
        set(_hash "")
        if (P_URL_HASH)
            if (NOT P_URL_HASH MATCHES "^([A-Za-z0-9]+)=([0-9A-Fa-f]+)$")
                message(FATAL_ERROR "EPM: ${name}: URL_HASH must look like SHA256=<hex>")
            endif()
            set(_algo "${CMAKE_MATCH_1}")
            string(TOLOWER "${CMAKE_MATCH_2}" _hash)
        elseif (P_URL)
            _epm_setting(_unverified EPM_ALLOW_UNVERIFIED)
            if (NOT P_ALLOW_UNVERIFIED AND NOT _unverified)
                message(FATAL_ERROR "EPM: ${name}: URL_HASH (ALGO=hex) is required. Pass ALLOW_UNVERIFIED "
                    "or set EPM_ALLOW_UNVERIFIED=ON to accept an unverified download.")
            endif()
        endif()

        # Everything that changes the content belongs in the cache key.
        set(_content "${_identity}|${P_SPARSE_PATHS}|${P_GIT_SUBMODULES}")
        set(_patches "")
        foreach (_patch IN LISTS P_PATCHES)
            get_filename_component(_patch "${_patch}" ABSOLUTE)
            if (NOT EXISTS "${_patch}")
                message(FATAL_ERROR "EPM: ${name}: patch file '${_patch}' does not exist")
            endif()
            file(SHA256 "${_patch}" _sum)
            string(APPEND _content "|${_sum}")
            list(APPEND _patches "${_patch}")
        endforeach()

        set(_pop OUT_DIR _src_dir OUT_FILE _file OUT_STATE _source
            FETCH_DIR "${P_FETCH_DIR}" FILENAME "${P_FILENAME}" ALGO "${_algo}" HASH "${_hash}"
            PATCHES ${_patches} SPARSE_PATHS ${P_SPARSE_PATHS} GIT_SUBMODULES ${P_GIT_SUBMODULES})
        foreach (_flag NO_CACHE NO_EXTRACT)
            if (P_${_flag})
                list(APPEND _pop ${_flag})
            endif()
        endforeach()
        if (P_GIT_REPOSITORY)
            list(APPEND _pop GIT_REPOSITORY "${P_GIT_REPOSITORY}" GIT_TAG "${P_GIT_TAG}" GIT_SHALLOW "${P_GIT_SHALLOW}")
        else()
            list(APPEND _pop URL "${P_URL}")
        endif()
        _epm_populate(${name} "${_content}" ${_pop})
    endif()

    # A tree inside the calling directory is built in the matching directory of the build tree,
    # as add_subdirectory would.
    if (_src_dir AND NOT _source STREQUAL local)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_src_dir}")
        file(RELATIVE_PATH _rel_to_build "${CMAKE_BINARY_DIR}" "${_src_dir}")
        # (across drives, file(RELATIVE_PATH) answers with the absolute path)
        if (_rel AND NOT IS_ABSOLUTE "${_rel}" AND NOT _rel MATCHES "^[.][.]"
                AND (IS_ABSOLUTE "${_rel_to_build}" OR _rel_to_build MATCHES "^[.][.]"))
            set(_bin_dir "${CMAKE_CURRENT_BINARY_DIR}/${_rel}")
        endif()
    endif()
    if (P_BINARY_DIR)
        get_filename_component(_bin_dir "${P_BINARY_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    if (NOT _source)
        if (NOT P_OPTIONAL)
            message(FATAL_ERROR "EPM: ${name} was not found and has no SOURCE_DIR, GIT_REPOSITORY or URL to fall back on")
        endif()
        message(STATUS "EPM: ${name} not found (optional)")
        set(_source none)
    elseif (NOT _source MATCHES "^(installed|target)$")
        _epm_apply_options(${name} ${P_OPTIONS})
        if (NOT P_DOWNLOAD_ONLY)
            _epm_add_subdirectory(${name} "${_src_dir}" "${P_SOURCE_SUBDIR}" "${_bin_dir}"
                "${P_EXCLUDE_FROM_ALL}" "${P_SYSTEM}")
        endif()
    endif()

    _epm_set(${name} REGISTERED TRUE)
    _epm_set(${name} KIND ${kind})
    _epm_set(${name} VERSION "${_version}")
    _epm_set(${name} SOURCE "${_source}")
    _epm_set(${name} SOURCE_DIR "${_src_dir}")
    _epm_set(${name} BINARY_DIR "${_bin_dir}")
    _epm_set(${name} FILE "${_file}")
    _epm_set(${name} IDENTITY "${_identity}")
    _epm_set(${name} REPO "${_orig_repo}")
    _epm_set(${name} REF "${P_GIT_TAG}")
    _epm_set(${name} URL "${_orig_url}")
    _epm_set(${name} HASH "${P_URL_HASH}")
    set_property(GLOBAL APPEND PROPERTY EPM_PACKAGES "${name}")
endfunction()

# Publishes the registry entry of the package just handled as variables in the caller of
# the public function it is used in.
macro(_epm_publish)
    get_property(_pub_name GLOBAL PROPERTY EPM_LAST)
    foreach (_k KIND SOURCE SOURCE_DIR BINARY_DIR VERSION FILE)
        _epm_get(_pub_${_k} ${_pub_name} ${_k})
    endforeach()
    if (_pub_SOURCE AND NOT _pub_SOURCE STREQUAL none)
        set(${_pub_name}_FOUND TRUE PARENT_SCOPE)
        set(${_pub_name}_ADDED TRUE PARENT_SCOPE)
    else()
        set(${_pub_name}_FOUND FALSE PARENT_SCOPE)
        set(${_pub_name}_ADDED FALSE PARENT_SCOPE)
    endif()
    set(${_pub_name}_SOURCE "${_pub_SOURCE}" PARENT_SCOPE)
    set(${_pub_name}_SOURCE_DIR "${_pub_SOURCE_DIR}" PARENT_SCOPE)
    set(${_pub_name}_BINARY_DIR "${_pub_BINARY_DIR}" PARENT_SCOPE)
    set(${_pub_name}_VERSION "${_pub_VERSION}" PARENT_SCOPE)
    if (_pub_KIND STREQUAL asset)
        set(${_pub_name}_DIR "${_pub_SOURCE_DIR}" PARENT_SCOPE)
        set(${_pub_name}_FILE "${_pub_FILE}" PARENT_SCOPE)
    endif()
    set(EPM_LAST_PACKAGE_NAME "${_pub_name}" PARENT_SCOPE)
endmacro()

# ======================================================================================
# public API
# ======================================================================================

function(epm_add_package)
    _epm_add(package FALSE ${ARGN})
    _epm_publish()
endfunction()

function(epm_find_package)
    _epm_add(package TRUE ${ARGN})
    _epm_publish()
endfunction()

function(epm_add_asset)
    _epm_add(asset FALSE ${ARGN})
    _epm_publish()
endfunction()

function(epm_declare_package)
    cmake_parse_arguments(D "" "NAME" "" ${ARGN})
    if (NOT D_NAME)
        message(FATAL_ERROR "epm_declare_package requires NAME")
    endif()
    set_property(GLOBAL PROPERTY EPM_DECLARED_${D_NAME} "${ARGN}")
endfunction()

# --- lock file ----------------------------------------------------------------------------

# Called from a lock file: the exact source of one package.
function(epm_lock_package)
    cmake_parse_arguments(K "" "NAME" "" ${ARGN})
    if (NOT K_NAME)
        message(FATAL_ERROR "epm_lock_package requires NAME")
    endif()
    set_property(GLOBAL PROPERTY EPM_LOCK_${K_NAME} "${ARGN}")
endfunction()

function(epm_use_package_lock file)
    get_filename_component(file "${file}" ABSOLUTE)
    if (EXISTS "${file}")
        include("${file}")
        return()
    endif()
    _epm_setting(_update EPM_UPDATE_LOCK_FILE)
    if (NOT _update)
        message(WARNING "EPM: package lock '${file}' does not exist; nothing is pinned. "
            "Create it with -DEPM_UPDATE_LOCK_FILE=${file}")
    endif()
endfunction()

# Fetched packages are written with the exact commit (git) or hash (URL); a download that
# had no hash gets the SHA256 of what was downloaded, so the lock makes it verified.
function(epm_write_package_lock file)
    find_package(Git QUIET)
    get_property(_names GLOBAL PROPERTY EPM_PACKAGES)
    set(_text "# Generated by epm_write_package_lock. Pins fetched packages to exact revisions.\n"
        "# Use: epm_use_package_lock(\"<this file>\") before the first epm_add_package.\n")
    foreach (_n IN LISTS _names)
        _epm_get(_source ${_n} SOURCE)
        if (NOT _source MATCHES "^(cached|downloaded)$")
            continue()
        endif()
        foreach (_k REPO REF URL HASH FILE SOURCE_DIR)
            _epm_get(_${_k} ${_n} ${_k})
        endforeach()
        if (_REPO)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_SOURCE_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _res ERROR_QUIET)
            if (_res EQUAL 0)
                list(APPEND _text "epm_lock_package(NAME ${_n} GIT_REPOSITORY \"${_REPO}\" GIT_TAG ${_sha})  # ${_REF}\n")
            endif()
        elseif (_URL)
            if (NOT _HASH AND EXISTS "${_FILE}")
                file(SHA256 "${_FILE}" _sum)
                set(_HASH "SHA256=${_sum}")
            endif()
            if (_HASH)
                list(APPEND _text "epm_lock_package(NAME ${_n} URL \"${_URL}\" URL_HASH ${_HASH})\n")
            endif()
        endif()
    endforeach()
    string(JOIN "" _text ${_text})
    file(WRITE "${file}" "${_text}")
    message(STATUS "EPM: wrote package lock ${file}")
endfunction()

# --- summary ------------------------------------------------------------------------------

function(epm_print_summary)
    get_property(_names GLOBAL PROPERTY EPM_PACKAGES)
    message(STATUS "EPM packages:")
    foreach (_n IN LISTS _names)
        foreach (_k KIND VERSION SOURCE SOURCE_DIR)
            _epm_get(_${_k} ${_n} ${_k})
        endforeach()
        string(LENGTH "${_n}" _len)
        math(EXPR _pad "24 - ${_len}")
        if (_pad LESS 1)
            set(_pad 1)
        endif()
        string(REPEAT " " ${_pad} _spaces)
        message(STATUS "  ${_n}${_spaces}${_KIND}  ${_VERSION}  [${_SOURCE}]  ${_SOURCE_DIR}")
    endforeach()
endfunction()

function(_epm_finalize)
    _epm_setting(_summary EPM_SHOW_SUMMARY)
    _epm_setting(_lock_file EPM_UPDATE_LOCK_FILE)
    if (_summary)
        epm_print_summary()
    endif()
    if (_lock_file)
        epm_write_package_lock("${_lock_file}")
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _epm_finalize)
