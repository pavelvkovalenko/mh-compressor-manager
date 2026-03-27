#!/bin/bash
#Сборка
cd src
mkdir -p build
cd build && make clean
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
