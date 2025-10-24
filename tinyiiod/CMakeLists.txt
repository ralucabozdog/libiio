set(TINYIIOD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/lib/libiio/tinyiiod)

add_library(tinyiiod STATIC
	${TINYIIOD_ROOT}/tinyiiod.c
	${TINYIIOD_ROOT}/zephiio.c
	${TINYIIOD_ROOT}/../iiod/responder.c
	${TINYIIOD_ROOT}/../iiod/rw.c
)
target_include_directories(tinyiiod
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
		${TINYIIOD_ROOT}/../iiod
		${TINYIIOD_ROOT}/..
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${TINYIIOD_ROOT}/../include
)

set_target_properties(tinyiiod PROPERTIES
	PUBLIC_HEADER tinyiiod.h
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)
