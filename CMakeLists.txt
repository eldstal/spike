# mkdir build
# cd build
# cmake -GNinja ..
# ninja

# Options
set(BOARD nrf52840dk_nrf52840)
#set(CONF_FILE prj.conf)
set(DTC_OVERLAY_FILE peripherals.overlay)


# Find Zephyr. This also loads Zephyr's build system.
cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr)
project(spike)

# Add your source file to the "app" target. This must come after
# find_package(Zephyr) which defines the target.
target_sources(app PRIVATE src/main.cpp)
