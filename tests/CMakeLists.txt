add_executable(tsl_hat_trie_tests "main.cpp" 
                                  "trie_map_tests.cpp" 
                                  "trie_set_tests.cpp"
                                  "trie_policies_tests.cpp"
                                  "trie_allocator_tests.cpp")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(tsl_hat_trie_tests PRIVATE -std=c++11 -Werror -Wall -Wextra -Wold-style-cast -O3 -DTSL_DEBUG)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(tsl_hat_trie_tests PRIVATE /bigobj /WX /W3 /DTSL_DEBUG)
endif()

# Boost::unit_test_framework
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.54.0 CONFIG REQUIRED COMPONENTS unit_test_framework)
target_link_libraries(tsl_hat_trie_tests PRIVATE Boost::unit_test_framework)   

# Boost.Interprocess (header only) for the shared memory / memory mapped files
# tests. Shared memory objects need pthread and, with older glibc, rt.
find_package(Threads REQUIRED)
target_link_libraries(tsl_hat_trie_tests PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    find_library(TSL_RT_LIBRARY rt)
    if(TSL_RT_LIBRARY)
        target_link_libraries(tsl_hat_trie_tests PRIVATE ${TSL_RT_LIBRARY})
    endif()
endif()

# tsl::hat_trie
target_link_libraries(tsl_hat_trie_tests PRIVATE tsl::hat_trie)  

add_test(tsl_hat_trie_tests tsl_hat_trie_test)
