idf_component_register(
	SRCS
		"main.c"
		"hal_esp32.c"
		"sharp_lcd.c"
		"qemu_lcd.c"
		"keymatrix.c"
		"serial_keys.c"
	INCLUDE_DIRS "."
	REQUIRES qdos_core driver esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag
		esp_timer fatfs vfs wear_levelling)

# The read-only system store, built from the programs shipped in the tree
get_filename_component(QDOS "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
fatfs_create_rawflash_image(system "${QDOS}/programs/system" FLASH_IN_PROJECT)
