# tests/CMakeLists.txt
# One executable per test file; ctest picks them up via add_test.
 
# Example — uncomment when you add your first test file:
add_executable(test_order test_order.cpp)
target_link_libraries(test_order PRIVATE
    me_core
    GTest::gtest_main
    project_warnings
    project_options
)
add_test(NAME order_tests COMMAND test_order)
