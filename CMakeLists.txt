cmake_minimum_required(VERSION 3.20)
project(AreaUpdate LANGUAGES CXX)

option(AREAUPDATE_TASK_STATISTICS "Enable Task lifetime diagnostics" ON)
add_compile_definitions(AREAUPDATE_TASK_STATISTICS=$<BOOL:${AREAUPDATE_TASK_STATISTICS}>)

add_library(Core STATIC Source/Core/Worker.cpp)
target_include_directories(Core PUBLIC Source/Core)
target_compile_features(Core PUBLIC cxx_std_14)
target_compile_definitions(Core PUBLIC NOMINMAX)

add_library(AreaUpdate STATIC
    Source/AreaUpdate/Area.cpp
    Source/AreaUpdate/AreaExecuter.cpp
)
target_include_directories(AreaUpdate PUBLIC Source/AreaUpdate)
target_link_libraries(AreaUpdate PUBLIC Core)

add_library(ObjectUpdate STATIC Source/ObjectUpdate/TaskExecuter.cpp)
target_include_directories(ObjectUpdate PUBLIC Source/ObjectUpdate)
target_link_libraries(ObjectUpdate PUBLIC Core)

add_library(AreaUpdateLegacy STATIC
    Source/AreaUpdateLegacy/OriginalAreaAdapter.cpp
    Source/AreaUpdateLegacy/Optimized/RadiusExecuter.cpp
)
target_link_libraries(AreaUpdateLegacy PUBLIC Core)
target_include_directories(AreaUpdateLegacy PUBLIC Source/AreaUpdateLegacy)
target_compile_features(AreaUpdateLegacy PUBLIC cxx_std_14)
target_compile_definitions(AreaUpdateLegacy PUBLIC NOMINMAX)

add_executable(AreaUpdateExample Source/Examples/AreaUpdateExample/main.cpp)
target_link_libraries(AreaUpdateExample PRIVATE AreaUpdate ObjectUpdate AreaUpdateLegacy)

add_executable(UpdateComparison
    Source/Benchmarks/UpdateComparison/UpdateComparison.cpp
    Source/Benchmarks/UpdateComparison/MotionInteractionProbe.cpp
)
target_link_libraries(UpdateComparison PRIVATE AreaUpdate ObjectUpdate AreaUpdateLegacy)
target_compile_features(UpdateComparison PRIVATE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
    # Hook-enabled Worker and its consumers must use the same class layout.
    add_executable(AreaTests
        Source/Tests/AreaTests/AreaTests.cpp
        Source/Tests/AreaTests/RuntimeTests.cpp
        Source/Tests/AreaTests/RadiusTests.cpp
        Source/AreaUpdateLegacy/Optimized/RadiusExecuter.cpp
        Source/AreaUpdate/Area.cpp
        Source/AreaUpdate/AreaExecuter.cpp
        Source/Core/Worker.cpp
    )
    target_include_directories(AreaTests PRIVATE Source/Core Source/AreaUpdate)
    target_compile_features(AreaTests PRIVATE cxx_std_17)
    target_compile_definitions(AreaTests PRIVATE AREAUPDATE_TEST_HOOKS NOMINMAX)
    add_test(NAME AreaLifecycle COMMAND AreaTests)
    set_tests_properties(AreaLifecycle PROPERTIES TIMEOUT 30)
    add_test(NAME UpdateComparisonSelfTest COMMAND UpdateComparison --test)
    set_tests_properties(UpdateComparisonSelfTest PROPERTIES TIMEOUT 120)
    add_test(NAME MovingActorEvents COMMAND UpdateComparison --motion-probe area 3 256 42 verify uniform)
    set_tests_properties(MovingActorEvents PROPERTIES TIMEOUT 30)
endif()

if(MSVC)
    foreach(target Core AreaUpdate ObjectUpdate AreaUpdateLegacy AreaUpdateExample UpdateComparison)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endforeach()
endif()
