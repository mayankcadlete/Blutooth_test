idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash driver esp_timer)

# ---------------------------------------------------------------------------
# Per-box configuration: read DEVICE_ID and SECRET_KEY from device_config.json
# (project root) and pass them to main.c as compile definitions.
# Edit only that JSON file when configuring a new box, then build + flash.
# ---------------------------------------------------------------------------
idf_build_get_property(project_dir PROJECT_DIR)
set(DEVICE_CONFIG_FILE "${project_dir}/device_config.json")

if(NOT EXISTS "${DEVICE_CONFIG_FILE}")
    message(FATAL_ERROR "Missing ${DEVICE_CONFIG_FILE} (needs \"device_id\" and \"secret_key\")")
endif()

# Re-run CMake automatically whenever the JSON file changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DEVICE_CONFIG_FILE}")

file(READ "${DEVICE_CONFIG_FILE}" device_config_json)
foreach(field device_id secret_key)
    string(JSON value ERROR_VARIABLE json_err GET "${device_config_json}" ${field})
    if(json_err)
        message(FATAL_ERROR "device_config.json: ${json_err}")
    endif()
    set(cfg_${field} "${value}")
endforeach()

# Same rules as the QR setup page (index.html)
string(LENGTH "${cfg_device_id}"  id_len)
string(LENGTH "${cfg_secret_key}" key_len)
if(NOT cfg_device_id MATCHES "^[A-Za-z0-9_-]+$" OR id_len LESS 3 OR id_len GREATER 20)
    message(FATAL_ERROR "device_id \"${cfg_device_id}\" must be 3-20 letters, numbers, _ or -")
endif()
if(NOT cfg_secret_key MATCHES "^[A-Za-z0-9]+$" OR key_len LESS 6 OR key_len GREATER 12)
    message(FATAL_ERROR "secret_key must be 6-12 letters/numbers")
endif()

message(STATUS "Box config: DEVICE_ID=${cfg_device_id}")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "DEVICE_ID=\"${cfg_device_id}\""
    "SECRET_KEY=\"${cfg_secret_key}\"")
