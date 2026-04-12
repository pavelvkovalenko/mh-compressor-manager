# Компиляция .po → .mo для каждого языка
# ТЗ §22.5 — Файлы переводов

if(NOT Gettext_FOUND OR NOT GETTEXT_MSGFMT_EXECUTABLE)
    message(STATUS "Gettext not found — skipping .mo compilation")
    return()
endif()

file(GLOB PO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.po")
foreach(PO_FILE ${PO_FILES})
    get_filename_component(LANG ${PO_FILE} NAME_WE)
    set(MO_FILE "${CMAKE_CURRENT_BINARY_DIR}/locale/${LANG}/LC_MESSAGES/mh-compressor-manager.mo")

    add_custom_command(
        OUTPUT ${MO_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/locale/${LANG}/LC_MESSAGES"
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${MO_FILE} ${PO_FILE}
        DEPENDS ${PO_FILE}
        COMMENT "Compiling ${LANG}.po → ${LANG}.mo"
    )
    add_custom_target(locale_${LANG} ALL DEPENDS ${MO_FILE})
    install(FILES ${MO_FILE} DESTINATION share/locale/${LANG}/LC_MESSAGES)
endforeach()

# Генерация .pot шаблона из исходных кодов
add_custom_target(update-pot
    COMMAND ${GETTEXT_XGETTEXT_EXECUTABLE}
        --from-code=UTF-8
        --keyword=_ --keyword=N_ --keyword=_fmt:1
        --output=${CMAKE_CURRENT_SOURCE_DIR}/mh-compressor-manager.pot
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/*.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Updating translations template (.pot)"
)
