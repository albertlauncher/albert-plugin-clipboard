cmake_minimum_required(VERSION 3.16)
project(clipboard VERSION 4.5)

find_package(Albert REQUIRED)

albert_plugin(
    INCLUDE PRIVATE $<TARGET_PROPERTY:albert::snippets,INTERFACE_INCLUDE_DIRECTORIES>
    QT Widgets
)
