cmake_minimum_required(VERSION 3.20)

add_library(anjeer_exchange STATIC
    src/order_book.cpp
    src/sequencer.cpp
    src/exchange_session.cpp
)

target_include_directories(anjeer_exchange PUBLIC include)
target_compile_features(anjeer_exchange PUBLIC cxx_std_17)
