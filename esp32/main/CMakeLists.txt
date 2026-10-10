idf_component_register(
	SRCS
		"main.c"
		"hal_esp32.c"
		"sharp_lcd.c"
		"qemu_lcd.c"
		"keymatrix.c"
		"serial_keys.c"
		"builtins.c"
		"upload.c"
	INCLUDE_DIRS "."
	REQUIRES qdos_core doom driver esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag
		esp_timer fatfs vfs wear_levelling)

# Only native.c asks for the table, and libqdos_core.a comes after libmain.a
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u qdos_native_builtins")

# The read-only system store, built from the programs shipped in the tree
get_filename_component(QDOS "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(system_dir "${QDOS}/programs/system")

# DOOM is an app folder in it, as it is on the Pi's card: the loop, the WAD, and
# libdoom.so -- here only a name, the engine being linked in. Staged when the
# project is configured, so a change to programs/system wants a reconfigure.
if(CONFIG_QDOS_DOOM)
	# Found as components/doom finds it
	if(DEFINED ENV{QDOS_DOOM})
		set(doom_host "$ENV{QDOS_DOOM}")
	else()
		set(doom_host "${QDOS}/../qdos-doom")
	endif()
	if(DEFINED ENV{DOOM_WAD})
		set(doom_wad "$ENV{DOOM_WAD}")
	else()
		set(doom_wad "${doom_host}/doom/doom1.wad")
	endif()
	if(NOT EXISTS "${doom_wad}")
		message(FATAL_ERROR "No WAD at ${doom_wad}.\n"
			"Put doom1.wad in ${doom_host}/doom, or set DOOM_WAD.")
	endif()

	set(system_dir "${CMAKE_CURRENT_BINARY_DIR}/system")
	file(REMOVE_RECURSE "${system_dir}")
	file(COPY "${QDOS}/programs/system/" DESTINATION "${system_dir}")
	file(COPY "${doom_host}/doom/main.qd" "${doom_wad}" DESTINATION "${system_dir}/doom")
	file(WRITE "${system_dir}/doom/libdoom.so" "Linked into the firmware; see esp32/main/builtins.c\n")
endif()

fatfs_create_rawflash_image(system "${system_dir}" FLASH_IN_PROJECT)
