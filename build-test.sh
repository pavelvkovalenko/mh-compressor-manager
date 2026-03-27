#!/bin/bash
# Проверка структуры проекта
cd ~/Projects/mh-compressor
ls -la src/
ls -la *.spec 2>/dev/null || echo "SPEC файл не найден в корне!"

# Проверка наличия файлов в src/
ls src/compressor-manager.conf src/mh-compressor-manager.service src/README.md src/README.html 2>/dev/null || echo "Некоторые файлы отсутствуют!"

# Убедитесь, что файлы есть в корне архива
tar -tzf ~/rpmbuild/SOURCES/mh-compressor-manager-1.0.0.tar.gz | head -20

# Должно содержать:
# mh-compressor-manager-1.0.0/compressor-manager.conf
# mh-compressor-manager-1.0.0/mh-compressor-manager.service
# mh-compressor-manager-1.0.0/README.md
# mh-compressor-manager-1.0.0/README.html
# mh-compressor-manager-1.0.0/LICENSE
# mh-compressor-manager-1.0.0/src/...

# Убедитесь, что в архиве нет артефактов
tar -tzf ~/rpmbuild/SOURCES/mh-compressor-manager-1.0.0.tar.gz | grep -E "(CMakeCache|CMakeFiles|\.o$|build/)" && echo "⚠️ Артефакты найдены!" || echo "✓ Архив чистый"

# Убедитесь, что CMakeLists.txt есть
tar -tzf ~/rpmbuild/SOURCES/mh-compressor-manager-1.0.0.tar.gz | grep "CMakeLists.txt" && echo "✓ CMakeLists.txt найден"

cd ~/Projects/mh-compressor

# Проверка структуры
echo "=== Структура проекта ==="
ls -la
echo ""
echo "=== Содержимое src/ ==="
ls -la src/
echo ""
echo "=== Проверка CMakeLists.txt ==="
ls -la src/CMakeLists.txt