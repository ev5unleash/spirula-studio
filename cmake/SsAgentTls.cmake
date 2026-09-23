if(TARGET ss_agent_tls)
    return()
endif()

function(_ss_agent_tls_add_mbedtls)
    include(FetchContent)
    FetchContent_Declare(ss_agent_mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)

    # Mbed TLS exposes these as cache options. Preserve the caller's cache and
    # keep its programs, tests, package rules, and shared libraries out of this
    # static transport dependency.
    set(_cache_options
        ENABLE_PROGRAMS
        ENABLE_TESTING
        DISABLE_PACKAGE_CONFIG_AND_INSTALL
        GEN_FILES
        USE_STATIC_MBEDTLS_LIBRARY
        USE_SHARED_MBEDTLS_LIBRARY
        LINK_WITH_PTHREAD
        LINK_WITH_TRUSTED_STORAGE
        MSVC_STATIC_RUNTIME
        UNSAFE_BUILD)
    foreach(_name IN LISTS _cache_options)
        if(DEFINED CACHE{${_name}})
            get_property(_value CACHE "${_name}" PROPERTY VALUE)
            get_property(_type CACHE "${_name}" PROPERTY TYPE)
            get_property(_help CACHE "${_name}" PROPERTY HELPSTRING)
            set(_ss_cache_had_${_name} TRUE)
            set(_ss_cache_value_${_name} "${_value}")
            set(_ss_cache_type_${_name} "${_type}")
            set(_ss_cache_help_${_name} "${_help}")
        else()
            set(_ss_cache_had_${_name} FALSE)
        endif()
    endforeach()

    set(MBEDTLS_AS_SUBPROJECT ON)
    set(MBEDTLS_TARGET_PREFIX ss_agent_tls_)
    set(_off_options
        ENABLE_PROGRAMS
        ENABLE_TESTING
        GEN_FILES
        USE_SHARED_MBEDTLS_LIBRARY
        LINK_WITH_PTHREAD
        LINK_WITH_TRUSTED_STORAGE
        MSVC_STATIC_RUNTIME
        UNSAFE_BUILD)
    foreach(_name IN LISTS _off_options)
        set(${_name} OFF)
        set(${_name} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(ss_agent_mbedtls)

    foreach(_name IN LISTS _cache_options)
        if(_ss_cache_had_${_name})
            set(${_name} "${_ss_cache_value_${_name}}"
                CACHE "${_ss_cache_type_${_name}}"
                "${_ss_cache_help_${_name}}" FORCE)
        else()
            unset(${_name} CACHE)
        endif()
    endforeach()
endfunction()

_ss_agent_tls_add_mbedtls()

find_package(Threads REQUIRED)
add_library(ss_agent_tls STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../src/app/AgentTls.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/app/AgentPairing.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/app/AgentTransfer.cpp")
target_include_directories(ss_agent_tls PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_features(ss_agent_tls PUBLIC cxx_std_17)
target_link_libraries(ss_agent_tls PRIVATE ss_agent_tls_mbedtls Threads::Threads)
if(WIN32)
    target_link_libraries(ss_agent_tls PRIVATE ws2_32 advapi32)
endif()
