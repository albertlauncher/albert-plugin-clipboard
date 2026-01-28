cmake_minimum_required(VERSION 3.16)
project(clipboard VERSION 4.9.0)

find_package(Albert REQUIRED)
find_package(QCoro6 REQUIRED COMPONENTS Coro)

albert_plugin(
    INCLUDE PRIVATE $<TARGET_PROPERTY:albert::snippets,INTERFACE_INCLUDE_DIRECTORIES>
    QT Widgets
    LINK PRIVATE QCoro6::Coro
)
