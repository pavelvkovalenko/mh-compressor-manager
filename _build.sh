#!/bin/bash
#Сборка
cd src
mkdir -p build
cd build
if [ -f Makefile ]; then
    make clean 2>/dev/null || true
fi
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
