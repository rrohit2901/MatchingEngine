# tests/CMakeLists.txt
# One executable per test file; ctest picks them up via add_test.
 
add_executable(test_order test_order.cpp)
target_link_libraries(test_order PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME order_tests COMMAND test_order)

add_executable(test_book_level test_book_level.cpp)
target_link_libraries(test_book_level PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME book_level_tests COMMAND test_book_level)

add_executable(test_order_book_side test_order_book_side.cpp)
target_link_libraries(test_order_book_side PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME order_book_side_tests COMMAND test_order_book_side)

add_executable(test_order_book test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME order_book_tests COMMAND test_order_book)

add_executable(test_matcher test_matcher.cpp)
target_link_libraries(test_matcher PRIVATE
    me_core
    me_ds_lock          # LockQueue for the event container
    Threads::Threads
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME matcher_tests COMMAND test_matcher)

add_executable(test_order_manager test_order_manager.cpp)
target_link_libraries(test_order_manager PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME order_manager_tests COMMAND test_order_manager)

add_executable(test_matching_engine test_matching_engine.cpp)
target_link_libraries(test_matching_engine PRIVATE
    me_core
    me_ds_lock          # LockQueue for the event container
    Threads::Threads
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME matching_engine_tests COMMAND test_matching_engine)

add_executable(test_risk_manager test_risk_manager.cpp)
target_link_libraries(test_risk_manager PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME risk_manager_tests COMMAND test_risk_manager)
