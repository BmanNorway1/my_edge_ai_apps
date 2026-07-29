Nordic Axon NPU Library

To integrate the Nordic Axon NPU library into your project, extract the zip and copy the contents into your projects and ensure the following lines are added to your CMakeLists.txt:
set(EXTRA_CONF_FILE
        ./ei-model/conf_overlay.conf
)
add_subdirectory(ei-model/edge-impulse-sdk/cmake/zephyr)
target_include_directories(app PRIVATE ei-model)
zephyr_include_directories(ei-model/nordic-axon-model)

The above example assumes the contents are copied into a folder called "ei-model" in the root of your project, and that the generated model files are in "ei-model/nordic-axon-model". Adjust the paths as necessary based on where you copy the files in your project structure.
