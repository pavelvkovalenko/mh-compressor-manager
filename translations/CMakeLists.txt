# Компиляция .po → .mo для каждого языка
# ТЗ §22.5 — Файлы переводов

if(Gettext_FOUND AND GETTEXT_MSGFMT_EXECUTABLE)
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
else()
    message(STATUS "Gettext not found — skipping .mo compilation")
endif()
