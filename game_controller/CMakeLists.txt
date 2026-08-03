cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(game_controller)

target_sources(app PRIVATE
  src/main.c
  src/hw_modules/button.c
  src/gesture/imu/imu.c

  src/ble/ble_nus.c

  src/engine/engine_controller.c
  src/engine/kws_engine.c
  src/engine/gesture_engine.c

  src/gesture/gesture.c
  src/gesture/inference_postprocessing.c
  src/gesture/nrf_edgeai_generated/nrf54lm20dk/Axon/nrf_edgeai_user_model.c

  src/kws/dmic.c
  src/kws/kws.c
  src/kws/Snake_94502_kws/nrf_edgeai_generated/nrf_edgeai_user_model.c
)

# dmic.h / kws.h are referenced as "dmic.h" / "kws.h" from within src/kws;
# common.h is referenced from button.c and the imu module.
target_include_directories(app PRIVATE src src/kws)
target_include_directories(app PRIVATE src/hw_modules)
target_include_directories(app PRIVATE src/gesture/imu)
