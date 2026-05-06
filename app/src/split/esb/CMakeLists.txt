# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_include_directories(app PRIVATE nrf_esb)

target_sources(app PRIVATE nrf_esb/esb.c)
target_sources_ifdef(CONFIG_HAS_HW_NRF_PPI app PRIVATE nrf_esb/esb_ppi.c)
target_sources_ifdef(CONFIG_HAS_HW_NRF_DPPIC app PRIVATE nrf_esb/esb_dppi.c)

if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE central.c)
else()
    target_sources(app PRIVATE peripheral.c)
endif()
